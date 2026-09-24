#include "xr.h"
#include "log.h"
#include "keys.h"
#include "stereo.h"
#include "shadow.h"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <initializer_list>
#include <Xinput.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include "fakepad.h"
#include "capture.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// The loader is loaded at runtime (SteamVR ships one), so no import lib.
static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;

#define XR_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetSystem) X(xrCreateSession) X(xrCreateReferenceSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrEnumerateSwapchainImages) \
    X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) \
    X(xrEndFrame) X(xrLocateViews) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
    X(xrReleaseSwapchainImage) X(xrEnumerateViewConfigurationViews) X(xrResultToString) \
    X(xrGetD3D11GraphicsRequirementsKHR) X(xrLocateSpace) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) \
    X(xrGetActionStateBoolean) X(xrCreateActionSpace) X(xrGetCurrentInteractionProfile) X(xrPathToString)

#define DECLARE(name) static PFN_##name name##_ = nullptr;
XR_FUNCS(DECLARE)
#undef DECLARE

enum class XrStage { Off, NeedInstance, NeedSystem, Running, Failed };

static XrStage g_stage = XrStage::Off;
static ID3D11Device* g_device = nullptr;
static wchar_t g_loaderPath[MAX_PATH] = {};
static DWORD g_nextRetry = 0;

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_viewSpace = XR_NULL_HANDLE;
static XrSpace g_localSpace = XR_NULL_HANDLE;
static XrFovf g_eyeFov[2] = {};
static bool g_recenterPending = false;
static bool g_depthExt = false;       // XR_KHR_composition_layer_depth enabled on the instance
// Comfort: in VR the viewpoint sits behind/above the game camera so the lead
// petal isn't right between the eyes (meters, reference frame; saved to ini).
static float g_camBack = 1.0f, g_camUp = 0.25f;
static wchar_t g_iniPath[MAX_PATH] = {};
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_sessionRunning = false;

struct EyeSwapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;
    bool everReleased = false;
};
static EyeSwapchain g_eyes[2];
static UINT g_size = 0;                 // square swapchain edge, pixels
static DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
static ID3D11Texture2D* g_resolve = nullptr; // for MSAA backbuffers
static ID3D11Texture2D* g_resolveRight = nullptr;
static uint64_t g_submitted = 0;

static const char* ResultStr(XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString_ && g_instance && XR_SUCCEEDED(xrResultToString_(g_instance, r, buf))) return buf;
    sprintf_s(buf, "XrResult(%d)", (int)r);
    return buf;
}

#define XR_CHECK(call) do { XrResult _r = (call); if (XR_FAILED(_r)) { Log("[xr] %s failed: %s", #call, ResultStr(_r)); return false; } } while (0)

void XrInit(ID3D11Device* device, const wchar_t* dllDir)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\vrmod.ini", dllDir);
    if (!GetPrivateProfileIntW(L"xr", L"enabled", 1, ini))
    {
        Log("[xr] disabled in vrmod.ini");
        return;
    }
    GetPrivateProfileStringW(L"xr", L"loader",
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll",
        g_loaderPath, MAX_PATH, ini);
    g_device = device;
    g_stage = XrStage::NeedInstance;
    wcscpy_s(g_iniPath, ini);
    wchar_t buf[32];
    GetPrivateProfileStringW(L"xr", L"cameraBack", L"1.0", buf, 32, ini); g_camBack = (float)_wtof(buf);
    GetPrivateProfileStringW(L"xr", L"cameraUp", L"0.25", buf, 32, ini); g_camUp = (float)_wtof(buf);
    Log("[xr] camera offset: back %.2f, up %.2f game units", g_camBack, g_camUp);
}

static bool LoadLoader()
{
    HMODULE loader = LoadLibraryW(g_loaderPath);
    if (!loader)
    {
        Log("[xr] could not load %ls (error %lu)", g_loaderPath, GetLastError());
        return false;
    }
    xrGetInstanceProcAddr_ = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr_) { Log("[xr] loader has no xrGetInstanceProcAddr"); return false; }
    return true;
}

// Fails while SteamVR is still starting (or no headset); caller retries.
static bool CreateInstance()
{

    PFN_xrCreateInstance xrCreateInstance_ = nullptr;
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance_);

    // Depth layers if the runtime offers them.
    PFN_xrEnumerateInstanceExtensionProperties enumExt = nullptr;
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumExt);
    g_depthExt = false;
    if (enumExt)
    {
        uint32_t n = 0;
        enumExt(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, { XR_TYPE_EXTENSION_PROPERTIES });
        enumExt(nullptr, n, &n, props.data());
        for (auto& p : props)
            if (!strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) g_depthExt = true;
    }
    wchar_t depthIni[8];
    GetPrivateProfileStringW(L"xr", L"depth", L"1", depthIni, 8, g_iniPath);
    if (depthIni[0] == L'0') g_depthExt = false;

    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "Flower VRMod");
    strcpy_s(ci.applicationInfo.engineName, "PhyreEngine (injected)");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = g_depthExt ? 2 : 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance_(&ci, &g_instance);
    if (XR_FAILED(r))
    {
        static XrResult lastLogged = XR_SUCCESS;
        if (r != lastLogged) Log("[xr] xrCreateInstance: %s (SteamVR starting or no headset?), will keep retrying", ResultStr(r));
        lastLogged = r;
        g_instance = XR_NULL_HANDLE;
        return false;
    }

#define LOAD(name) xrGetInstanceProcAddr_(g_instance, #name, (PFN_xrVoidFunction*)&name##_); \
    if (!name##_) { Log("[xr] missing function " #name); return false; }
    XR_FUNCS(LOAD)
#undef LOAD
    Log("[xr] instance created");
    return true;
}

// Returns false while no HMD is available (caller retries).
static bool GetSystem()
{
    XrSystemGetInfo si{ XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem_(g_instance, &si, &g_system);
    if (XR_FAILED(r))
    {
        static XrResult lastLogged = XR_SUCCESS;
        if (r != lastLogged) Log("[xr] no headset yet (%s), will keep retrying", ResultStr(r));
        lastLogged = r;
        return false;
    }
    return true;
}

static DXGI_FORMAT SrgbOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

static bool CreateInput(); // motion controllers, below
static bool CreateDepthResources(const std::vector<int64_t>& formats); // depth layers, below

static bool CreateSession(IDXGISwapChain* gameSwapChain)
{
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XR_CHECK(xrGetD3D11GraphicsRequirementsKHR_(g_instance, g_system, &req));

    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC ad;
            adapter->GetDesc(&ad);
            bool same = ad.AdapterLuid.LowPart == req.adapterLuid.LowPart && ad.AdapterLuid.HighPart == req.adapterLuid.HighPart;
            Log("[xr] game adapter '%ls' %s the headset's adapter", ad.Description, same ? "matches" : "DOES NOT match");
            adapter->Release();
        }
        dxgiDev->Release();
    }

    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = g_device;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = g_system;
    XR_CHECK(xrCreateSession_(g_instance, &sci, &g_session));

    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace_(g_session, &rs, &g_viewSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace_(g_session, &rs, &g_localSpace));

    // Swapchain format: sRGB twin of the game's backbuffer format, so a raw
    // copy is legal (same typeless family) and the compositor decodes gamma.
    ID3D11Texture2D* bb = nullptr;
    gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_TEXTURE2D_DESC bbDesc = {};
    bb->GetDesc(&bbDesc);
    bb->Release();
    DXGI_FORMAT want = SrgbOf(bbDesc.Format);

    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainFormats_(g_session, 0, &n, nullptr));
    std::vector<int64_t> formats(n);
    XR_CHECK(xrEnumerateSwapchainFormats_(g_session, n, &n, formats.data()));
    for (int64_t f : formats) if (f == want || f == bbDesc.Format) { g_format = (DXGI_FORMAT)f; if (f == want) break; }
    if (g_format == DXGI_FORMAT_UNKNOWN)
    {
        Log("[xr] runtime offers no format compatible with backbuffer format %d", (int)bbDesc.Format);
        return false;
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews_(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    XR_CHECK(xrEnumerateViewConfigurationViews_(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()));

    // Square crop of the game's frame, as big as its shorter side.
    g_size = bbDesc.Width < bbDesc.Height ? bbDesc.Width : bbDesc.Height;
    Log("[xr] backbuffer %ux%u fmt=%d samples=%u; swapchain %ux%u fmt=%d; HMD recommends %ux%u per eye",
        bbDesc.Width, bbDesc.Height, (int)bbDesc.Format, bbDesc.SampleDesc.Count, g_size, g_size, (int)g_format,
        views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);

    for (int e = 0; e < 2; ++e)
    {
        XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = g_format;
        sc.sampleCount = 1;
        sc.width = g_size;
        sc.height = g_size;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XR_CHECK(xrCreateSwapchain_(g_session, &sc, &g_eyes[e].handle));

        uint32_t count = 0;
        XR_CHECK(xrEnumerateSwapchainImages_(g_eyes[e].handle, 0, &count, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> imgs(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        XR_CHECK(xrEnumerateSwapchainImages_(g_eyes[e].handle, count, &count, (XrSwapchainImageBaseHeader*)imgs.data()));
        for (auto& i : imgs) g_eyes[e].images.push_back(i.texture);
    }

    if (g_depthExt && Stereo().doubleRender && !CreateDepthResources(formats))
        Log("[xr] depth submission unavailable (continuing without it)");

    if (bbDesc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC rd = bbDesc;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
        rd.BindFlags = 0; rd.MiscFlags = 0;
        g_device->CreateTexture2D(&rd, nullptr, &g_resolve);
        g_device->CreateTexture2D(&rd, nullptr, &g_resolveRight);
    }

    if (!CreateInput()) Log("[xr] controller input unavailable (continuing without it)");
    Log("[xr] session created, waiting for READY");
    return true;
}

static void PollEvents()
{
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent_(g_instance, &ev) == XR_SUCCESS)
    {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            g_state = s->state;
            Log("[xr] session state -> %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession_(g_session, &bi);
                g_sessionRunning = XR_SUCCEEDED(r);
                Log("[xr] xrBeginSession: %s", ResultStr(r));
            }
            else if (g_state == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession_(g_session);
                g_sessionRunning = false;
                Log("[xr] session ended");
            }
            else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING)
            {
                g_sessionRunning = false;
                g_stage = XrStage::Failed; // TODO: tear down and recreate
                Log("[xr] session lost/exiting; VR output stopped");
            }
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            g_recenterPending = true; // user recentered in SteamVR
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            g_sessionRunning = false;
            g_stage = XrStage::Failed;
            Log("[xr] instance loss pending; VR output stopped");
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Copy the centered square of the backbuffer into eye `e`'s next image.
static void CopyToEye(int e, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& desc)
{
    EyeSwapchain& sc = g_eyes[e];
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(sc.handle, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(sc.handle, &wi);

    D3D11_BOX box;
    box.left = (desc.Width - g_size) / 2; box.right = box.left + g_size;
    box.top = (desc.Height - g_size) / 2; box.bottom = box.top + g_size;
    box.front = 0; box.back = 1;
    ctx->CopySubresourceRegion(sc.images[idx], 0, 0, 0, 0, src, 0, &box);

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(sc.handle, &ri);
    sc.everReleased = true;
}

// ---- motion controllers -> virtual gamepad ----
// Default "motion" steering: point the right controller (left if the right
// isn't tracked); straight ahead is wherever it pointed at the last re-center
// (thumbstick click, or automatically the first time), 30 degrees = full stick.
// The thumbstick also steers (whichever is deflected more). B/Y switch to
// "stick" steering (thumbstick only) and back; saved as [xr] steering.
// Hold trigger, grip, A or X to fly (Flower's "any button"); menu = Start.
static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction g_actSteer, g_actTrigger, g_actGrip, g_actFlyButton, g_actToggle, g_actRecenterAim, g_actMenu, g_actAim;
static XrPath g_hand[2] = {};
static XrSpace g_aimSpace[2] = {};
static bool g_inputReady = false;
static bool g_motionSteering = true;

static XrPath ToPath(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath_(g_instance, s, &p);
    return p;
}

static XrAction MakeAction(const char* name, const char* label, XrActionType type)
{
    XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, label);
    ci.actionType = type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = g_hand;
    XrAction a = XR_NULL_HANDLE;
    XrResult r = xrCreateAction_(g_actionSet, &ci, &a);
    if (XR_FAILED(r)) Log("[xr] xrCreateAction %s: %s", name, ResultStr(r));
    return a;
}

struct SuggestedBinding { XrAction* action; const char* path; };

static void Suggest(const char* profile, std::initializer_list<SuggestedBinding> list)
{
    std::vector<XrActionSuggestedBinding> b;
    for (const SuggestedBinding& s : list)
        b.push_back({ *s.action, ToPath(s.path) });
    XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = ToPath(profile);
    sb.suggestedBindings = b.data();
    sb.countSuggestedBindings = (uint32_t)b.size();
    XrResult r = xrSuggestInteractionProfileBindings_(g_instance, &sb);
    Log("[xr] bindings for %s: %s", profile, ResultStr(r));
}

static bool CreateInput()
{
    g_hand[0] = ToPath("/user/hand/left");
    g_hand[1] = ToPath("/user/hand/right");

    wchar_t mode[32];
    GetPrivateProfileStringW(L"xr", L"steering", L"motion", mode, 32, g_iniPath);
    g_motionSteering = _wcsicmp(mode, L"stick") != 0;
    Log("[xr] steering: %s", g_motionSteering ? "motion" : "stick");

    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "flower");
    strcpy_s(asci.localizedActionSetName, "Flower");
    XR_CHECK(xrCreateActionSet_(g_instance, &asci, &g_actionSet));

    g_actSteer = MakeAction("steer", "Steer (thumbstick)", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_actTrigger = MakeAction("fly_trigger", "Fly (trigger)", XR_ACTION_TYPE_FLOAT_INPUT);
    g_actGrip = MakeAction("fly_grip", "Fly (grip)", XR_ACTION_TYPE_FLOAT_INPUT);
    g_actFlyButton = MakeAction("fly_button", "Fly (button)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_actToggle = MakeAction("steering_mode", "Switch motion / thumbstick steering", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_actRecenterAim = MakeAction("recenter_steering", "Re-center motion steering", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_actMenu = MakeAction("menu", "Pause", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_actAim = MakeAction("aim", "Hand aim", XR_ACTION_TYPE_POSE_INPUT);

    Suggest("/interaction_profiles/oculus/touch_controller", {
        { &g_actSteer, "/user/hand/left/input/thumbstick" }, { &g_actSteer, "/user/hand/right/input/thumbstick" },
        { &g_actTrigger, "/user/hand/left/input/trigger/value" }, { &g_actTrigger, "/user/hand/right/input/trigger/value" },
        { &g_actGrip, "/user/hand/left/input/squeeze/value" }, { &g_actGrip, "/user/hand/right/input/squeeze/value" },
        { &g_actFlyButton, "/user/hand/right/input/a/click" }, { &g_actFlyButton, "/user/hand/left/input/x/click" },
        { &g_actToggle, "/user/hand/right/input/b/click" }, { &g_actToggle, "/user/hand/left/input/y/click" },
        { &g_actRecenterAim, "/user/hand/left/input/thumbstick/click" }, { &g_actRecenterAim, "/user/hand/right/input/thumbstick/click" },
        { &g_actMenu, "/user/hand/left/input/menu/click" },
        { &g_actAim, "/user/hand/left/input/aim/pose" }, { &g_actAim, "/user/hand/right/input/aim/pose" } });
    Suggest("/interaction_profiles/valve/index_controller", {
        { &g_actSteer, "/user/hand/left/input/thumbstick" }, { &g_actSteer, "/user/hand/right/input/thumbstick" },
        { &g_actTrigger, "/user/hand/left/input/trigger/value" }, { &g_actTrigger, "/user/hand/right/input/trigger/value" },
        { &g_actGrip, "/user/hand/left/input/squeeze/value" }, { &g_actGrip, "/user/hand/right/input/squeeze/value" },
        { &g_actFlyButton, "/user/hand/left/input/a/click" }, { &g_actFlyButton, "/user/hand/right/input/a/click" },
        { &g_actToggle, "/user/hand/right/input/b/click" }, { &g_actMenu, "/user/hand/left/input/b/click" },
        { &g_actRecenterAim, "/user/hand/left/input/thumbstick/click" }, { &g_actRecenterAim, "/user/hand/right/input/thumbstick/click" },
        { &g_actAim, "/user/hand/left/input/aim/pose" }, { &g_actAim, "/user/hand/right/input/aim/pose" } });
    Suggest("/interaction_profiles/htc/vive_controller", {
        { &g_actSteer, "/user/hand/left/input/trackpad" }, { &g_actSteer, "/user/hand/right/input/trackpad" },
        { &g_actTrigger, "/user/hand/left/input/trigger/value" }, { &g_actTrigger, "/user/hand/right/input/trigger/value" },
        { &g_actFlyButton, "/user/hand/left/input/squeeze/click" }, { &g_actFlyButton, "/user/hand/right/input/squeeze/click" },
        { &g_actRecenterAim, "/user/hand/left/input/trackpad/click" }, { &g_actRecenterAim, "/user/hand/right/input/trackpad/click" },
        { &g_actToggle, "/user/hand/right/input/menu/click" }, { &g_actMenu, "/user/hand/left/input/menu/click" },
        { &g_actAim, "/user/hand/left/input/aim/pose" }, { &g_actAim, "/user/hand/right/input/aim/pose" } });
    Suggest("/interaction_profiles/microsoft/motion_controller", {
        { &g_actSteer, "/user/hand/left/input/thumbstick" }, { &g_actSteer, "/user/hand/right/input/thumbstick" },
        { &g_actTrigger, "/user/hand/left/input/trigger/value" }, { &g_actTrigger, "/user/hand/right/input/trigger/value" },
        { &g_actFlyButton, "/user/hand/left/input/squeeze/click" }, { &g_actFlyButton, "/user/hand/right/input/squeeze/click" },
        { &g_actRecenterAim, "/user/hand/left/input/thumbstick/click" }, { &g_actRecenterAim, "/user/hand/right/input/thumbstick/click" },
        { &g_actToggle, "/user/hand/right/input/menu/click" }, { &g_actMenu, "/user/hand/left/input/menu/click" },
        { &g_actAim, "/user/hand/left/input/aim/pose" }, { &g_actAim, "/user/hand/right/input/aim/pose" } });
    Suggest("/interaction_profiles/khr/simple_controller", {
        { &g_actFlyButton, "/user/hand/left/input/select/click" }, { &g_actFlyButton, "/user/hand/right/input/select/click" },
        { &g_actMenu, "/user/hand/left/input/menu/click" }, { &g_actMenu, "/user/hand/right/input/menu/click" },
        { &g_actAim, "/user/hand/left/input/aim/pose" }, { &g_actAim, "/user/hand/right/input/aim/pose" } });

    for (int h = 0; h < 2; ++h)
    {
        XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = g_actAim;
        sci.subactionPath = g_hand[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace_(g_session, &sci, &g_aimSpace[h]));
    }

    XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    XR_CHECK(xrAttachSessionActionSets_(g_session, &ai));
    g_inputReady = true;
    Log("[xr] controller input ready");
    return true;
}

static float GetFloat(XrAction a, int h)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = g_hand[h];
    XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat_(g_session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static bool GetBool(XrAction a, int h, bool* pressedNow = nullptr)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = g_hand[h];
    XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
    bool ok = XR_SUCCEEDED(xrGetActionStateBoolean_(g_session, &gi, &st)) && st.isActive;
    if (pressedNow) *pressedNow = ok && st.currentState && st.changedSinceLastSync;
    return ok && st.currentState;
}
static XrVector2f GetVec2(XrAction a, int h)
{
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = g_hand[h];
    XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_SUCCEEDED(xrGetActionStateVector2f_(g_session, &gi, &st)) && st.isActive) return st.currentState;
    return { 0, 0 };
}

static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b);
static XrQuaternionf QConj(const XrQuaternionf& q);
static XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v);

static void Clamp1(float& v) { if (v > 1) v = 1; if (v < -1) v = -1; }

static void PollControllers(XrTime time)
{
    if (!g_inputReady || g_state != XR_SESSION_STATE_FOCUSED) { FakePadSetXR(0, 0, 0, 0, 0); return; }
    XrActiveActionSet active{ g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult sr = xrSyncActions_(g_session, &si);
    if (XR_FAILED(sr) || sr == XR_SESSION_NOT_FOCUSED)
    {
        static XrResult last = XR_SUCCESS;
        if (sr != last) Log("[xr] xrSyncActions: %s", ResultStr(sr));
        last = sr;
        if (XR_FAILED(sr)) return;
    }

    float stickX = 0, stickY = 0, fly = 0;
    bool menu = false, toggle = false, recenter = false;
    XrPosef aim[2];
    bool aimOk[2] = {};
    for (int h = 0; h < 2; ++h)
    {
        XrVector2f s = GetVec2(g_actSteer, h);
        if (s.x * s.x + s.y * s.y > stickX * stickX + stickY * stickY) { stickX = s.x; stickY = s.y; }
        float t = GetFloat(g_actTrigger, h), g = GetFloat(g_actGrip, h);
        if (t > fly) fly = t;
        if (g > fly) fly = g;
        if (GetBool(g_actFlyButton, h)) fly = 1.0f;
        menu |= GetBool(g_actMenu, h);
        bool p = false;
        // Steering-mode switch needs a 1 s hold (a tap on B/Y flipped it by accident).
        static DWORD holdStart[2] = {};
        static bool holdFired[2] = {};
        if (GetBool(g_actToggle, h))
        {
            if (!holdStart[h]) { holdStart[h] = GetTickCount(); holdFired[h] = false; }
            if (!holdFired[h] && GetTickCount() - holdStart[h] >= 1000) { toggle = true; holdFired[h] = true; }
        }
        else holdStart[h] = 0;
        GetBool(g_actRecenterAim, h, &p); recenter |= p;

        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        aimOk[h] = XR_SUCCEEDED(xrLocateSpace_(g_aimSpace[h], g_localSpace, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
        aim[h] = loc.pose;
    }

    if (toggle)
    {
        g_motionSteering = !g_motionSteering;
        WritePrivateProfileStringW(L"xr", L"steering", g_motionSteering ? L"motion" : L"stick", g_iniPath);
        Log("[xr] steering switched to %s", g_motionSteering ? "motion" : "stick");
    }

    // Motion steering: the right hand (left if the right isn't tracked), only
    // in a 3D level (the menu expects no stick input to settle on a pot).
    // Pressing a fly button sets "straight" to wherever the controller points
    // at that moment, and pointing steers while it's held - like the grip
    // version, on the fly buttons. Thumbstick click re-centers mid-hold; a
    // pushed thumbstick overrides motion.
    static bool neutralSet[2] = {};
    static XrQuaternionf neutral[2];
    static bool wasFlying = false;
    bool inLevel = StereoProjectionFresh();
    bool flying = fly > 0.5f;
    if (flying && !wasFlying) { neutralSet[0] = neutralSet[1] = false; }
    wasFlying = flying;

    float lx = stickX, ly = stickY;
    int h = aimOk[1] ? 1 : aimOk[0] ? 0 : -1;
    if (h >= 0 && inLevel && flying)
    {
        if (recenter || !neutralSet[h])
        {
            neutral[h] = aim[h].orientation;
            neutralSet[h] = true;
        }
        bool stickPushed = stickX * stickX + stickY * stickY > 0.25f * 0.25f;
        if (g_motionSteering && !stickPushed)
        {
            XrVector3f f = QRot(QMul(QConj(neutral[h]), aim[h].orientation), { 0, 0, -1 });
            float mx = f.x / 0.5f, my = f.y / 0.5f; // sin(30 deg) = full deflection
            Clamp1(mx); Clamp1(my);
            lx = mx; ly = my;
        }
    }

    // Diagnostics: which controller type SteamVR reports, and live values.
    static DWORD lastDiag = 0;
    if (GetTickCount() - lastDiag > 3000)
    {
        lastDiag = GetTickCount();
        char prof[2][XR_MAX_PATH_LENGTH] = { "none", "none" };
        for (int i = 0; i < 2; ++i)
        {
            XrInteractionProfileState ps{ XR_TYPE_INTERACTION_PROFILE_STATE };
            if (XR_SUCCEEDED(xrGetCurrentInteractionProfile_(g_session, g_hand[i], &ps)) && ps.interactionProfile != XR_NULL_PATH)
            {
                uint32_t n = 0;
                xrPathToString_(g_instance, ps.interactionProfile, XR_MAX_PATH_LENGTH, &n, prof[i]);
            }
        }
        Log("[xr] controllers: left=%s right=%s | %s steering | out stick (%.2f %.2f) fly %.2f menu=%d",
            prof[0], prof[1], g_motionSteering ? "motion" : "stick", lx, ly, fly, menu);
    }

    WORD buttons = 0;
    if (fly > 0.5f) buttons |= XINPUT_GAMEPAD_A;
    if (menu) buttons |= XINPUT_GAMEPAD_START;
    FakePadSetXR(lx, ly, buttons, 0, (BYTE)(fly * 255.0f));
}

// ---- small quaternion helpers (x, y, z, w) ----
static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static XrQuaternionf QConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }
static XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
    XrQuaternionf p{ v.x, v.y, v.z, 0 };
    XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    return { r.x, r.y, r.z };
}
static void QToMat(const XrQuaternionf& q, float m[9]) // row-major
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// Reference ("where the game camera is") in LOCAL space: yaw-only
// orientation + position of the head at recenter time.
static bool g_refSet = false;
static XrPosef g_ref;
// Eye poses (LOCAL space) handed to the game for the frame being rendered
// now, and the ones each swapchain image was actually rendered with.
static XrPosef g_givenPose[2];
static bool g_givenValid = false;
static XrPosef g_imagePose[2];
static bool g_imagePoseValid[2] = {};

static void Recenter(const XrPosef& head)
{
    XrVector3f f = QRot(head.orientation, { 0, 0, -1 });
    float yaw = atan2f(-f.x, -f.z);
    g_ref.orientation = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
    g_ref.position = head.position;
    g_refSet = true;
    Log("[xr] recentered: yaw %.1f deg, head at (%.2f %.2f %.2f)", yaw * 57.2958f,
        head.position.x, head.position.y, head.position.z);
}

// ---- depth for the compositor (XR_KHR_composition_layer_depth) ----
// The runtime reprojects each frame to the newest head pose; without depth it
// has to guess distances, which showed as wobbly 'water' patches while the
// head moved. Each eye's scene depth (the game's 4x MSAA D24S8 buffer, or its
// shadow twin for the right eye) is copied - sample 0 - into a D32 depth
// swapchain by a fullscreen pass that runs in its own device-context state,
// so the game's pipeline state is untouched.
static bool g_depthReady = false;     // swapchains + shaders created
static EyeSwapchain g_depthSc[2];
static std::vector<ID3D11DepthStencilView*> g_depthDsv[2];
static ID3D11VertexShader* g_fsVS = nullptr;
static ID3D11PixelShader* g_depthPSms = nullptr;
static ID3D11PixelShader* g_depthPS1 = nullptr;
static ID3D11DepthStencilState* g_dssWrite = nullptr;
static ID3D11RasterizerState* g_rsNoCull = nullptr;
static ID3DDeviceContextState* g_ownState = nullptr;
static bool g_depthWritten[2] = {};

static const char* kFullscreenVS =
    "float4 main(uint id : SV_VertexID) : SV_Position {"
    "  float2 uv = float2((id << 1) & 2, id & 2);"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
static const char* kDepthPSms =
    "Texture2DMS<float> d : register(t0);"
    "float main(float4 p : SV_Position) : SV_Depth { return d.Load(int2(p.xy), 0); }";
static const char* kDepthPS1 =
    "Texture2D<float> d : register(t0);"
    "float main(float4 p : SV_Position) : SV_Depth { return d.Load(int3(p.xy, 0)); }";

static ID3DBlob* CompileHlsl(const char* src, const char* target)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    if (FAILED(hr)) Log("[xr] shader compile failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
    if (errors) errors->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

static bool CreateDepthResources(const std::vector<int64_t>& formats)
{
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (int64_t f : formats) if (f == DXGI_FORMAT_D32_FLOAT) fmt = DXGI_FORMAT_D32_FLOAT;
    if (fmt == DXGI_FORMAT_UNKNOWN)
        for (int64_t f : formats) if (f == DXGI_FORMAT_D24_UNORM_S8_UINT) fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (fmt == DXGI_FORMAT_UNKNOWN) { Log("[xr] depth: no depth swapchain format offered"); return false; }

    for (int e = 0; e < 2; ++e)
    {
        XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        sc.format = fmt;
        sc.sampleCount = 1;
        sc.width = g_size; sc.height = g_size;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        XR_CHECK(xrCreateSwapchain_(g_session, &sc, &g_depthSc[e].handle));
        uint32_t count = 0;
        XR_CHECK(xrEnumerateSwapchainImages_(g_depthSc[e].handle, 0, &count, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> imgs(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        XR_CHECK(xrEnumerateSwapchainImages_(g_depthSc[e].handle, count, &count, (XrSwapchainImageBaseHeader*)imgs.data()));
        for (auto& i : imgs)
        {
            g_depthSc[e].images.push_back(i.texture);
            D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
            dd.Format = fmt;
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            ID3D11DepthStencilView* dsv = nullptr;
            g_device->CreateDepthStencilView(i.texture, &dd, &dsv);
            g_depthDsv[e].push_back(dsv);
        }
    }

    ID3DBlob* vs = CompileHlsl(kFullscreenVS, "vs_5_0");
    ID3DBlob* psms = CompileHlsl(kDepthPSms, "ps_5_0");
    ID3DBlob* ps1 = CompileHlsl(kDepthPS1, "ps_5_0");
    if (!vs || !psms || !ps1) return false;
    g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_fsVS);
    g_device->CreatePixelShader(psms->GetBufferPointer(), psms->GetBufferSize(), nullptr, &g_depthPSms);
    g_device->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &g_depthPS1);
    vs->Release(); psms->Release(); ps1->Release();

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_device->CreateDepthStencilState(&ds, &g_dssWrite);
    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rs, &g_rsNoCull);

    ID3D11Device1* dev1 = nullptr;
    if (FAILED(g_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { Log("[xr] depth: no ID3D11Device1"); return false; }
    D3D_FEATURE_LEVEL fl = g_device->GetFeatureLevel();
    HRESULT hr = dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_ownState);
    dev1->Release();
    if (FAILED(hr)) { Log("[xr] depth: CreateDeviceContextState failed 0x%08X", (unsigned)hr); return false; }

    g_depthReady = g_fsVS && g_depthPSms && g_depthPS1 && g_dssWrite && g_rsNoCull;
    Log("[xr] depth submission %s (format %d)", g_depthReady ? "ready" : "FAILED", (int)fmt);
    return g_depthReady;
}

// Copy one eye's scene depth into its depth swapchain.
static void WriteDepth(int e, ID3D11DeviceContext* ctx, ID3D11Resource* depth)
{
    g_depthWritten[e] = false;
    if (!g_depthReady || !depth) return;
    D3D11_TEXTURE2D_DESC td = {};
    static_cast<ID3D11Texture2D*>(depth)->GetDesc(&td);
    if (td.Width != g_size || td.Height != g_size || td.Format != DXGI_FORMAT_R24G8_TYPELESS || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        static bool logged = false;
        if (!logged) { logged = true; Log("[xr] depth: unsupported scene depth %ux%u fmt %d bind 0x%X", td.Width, td.Height, (int)td.Format, td.BindFlags); }
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    bool ms = td.SampleDesc.Count > 1;
    sd.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (!ms) sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_device->CreateShaderResourceView(depth, &sd, &srv)) || !srv) return;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g_depthSc[e].handle, &ai, &idx))) { srv->Release(); return; }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(g_depthSc[e].handle, &wi);

    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
    {
        ID3DDeviceContextState* gameState = nullptr;
        ctx1->SwapDeviceContextState(g_ownState, &gameState);
        D3D11_VIEWPORT vp = { 0, 0, (float)g_size, (float)g_size, 0, 1 };
        ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx1->IASetInputLayout(nullptr);
        ctx1->VSSetShader(g_fsVS, nullptr, 0);
        ctx1->PSSetShader(ms ? g_depthPSms : g_depthPS1, nullptr, 0);
        ctx1->PSSetShaderResources(0, 1, &srv);
        ctx1->OMSetRenderTargets(0, nullptr, g_depthDsv[e][idx]);
        ctx1->OMSetDepthStencilState(g_dssWrite, 0);
        ctx1->RSSetState(g_rsNoCull);
        ctx1->RSSetViewports(1, &vp);
        ctx1->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx1->PSSetShaderResources(0, 1, &none);
        ctx1->OMSetRenderTargets(0, nullptr, nullptr);
        ctx1->SwapDeviceContextState(gameState, nullptr);
        if (gameState) gameState->Release();
        ctx1->Release();
        g_depthWritten[e] = true;
    }
    srv->Release();

    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g_depthSc[e].handle, &ri);
}

// ---- virtual screen for menus / title / videos ----
// When the game shows no 3D camera, its whole image goes onto a quad floating
// in front of the (recentered) user.
static const float kScreenDistance = 2.0f, kScreenWidth = 2.4f;
static EyeSwapchain g_screen;
static UINT g_screenW = 0, g_screenH = 0;

static bool SubmitScreen(IDXGISwapChain* swapChain, const XrFrameState& fs, XrCompositionLayerQuad& layer)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    bb->GetDesc(&desc);

    if (!g_screen.handle)
    {
        XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = g_format;
        sc.sampleCount = 1;
        sc.width = desc.Width;
        sc.height = desc.Height;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        XrResult r = xrCreateSwapchain_(g_session, &sc, &g_screen.handle);
        if (XR_FAILED(r))
        {
            Log("[xr] screen swapchain: %s", ResultStr(r));
            g_screen.handle = XR_NULL_HANDLE;
            bb->Release();
            return false;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages_(g_screen.handle, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages_(g_screen.handle, count, &count, (XrSwapchainImageBaseHeader*)imgs.data());
        for (auto& i : imgs) g_screen.images.push_back(i.texture);
        g_screenW = desc.Width; g_screenH = desc.Height;
        Log("[xr] virtual screen %ux%u for menus/videos", g_screenW, g_screenH);
    }

    // Keep the screen in front of the user: recenter once if never done.
    XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
    if (!g_refSet && XR_SUCCEEDED(xrLocateSpace_(g_viewSpace, g_localSpace, fs.predictedDisplayTime, &head)) &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        Recenter(head.pose);
    if (!g_refSet) { bb->Release(); return false; }

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(g_screen.handle, &ai, &idx))) { bb->Release(); return false; }
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(g_screen.handle, &wi);
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    ID3D11Texture2D* src = bb;
    if (g_resolve && desc.SampleDesc.Count > 1)
    {
        ctx->ResolveSubresource(g_resolve, 0, bb, 0, desc.Format);
        src = g_resolve;
    }
    if (desc.Width == g_screenW && desc.Height == g_screenH)
        ctx->CopySubresourceRegion(g_screen.images[idx], 0, 0, 0, 0, src, 0, nullptr);
    ctx->Release();
    bb->Release();
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(g_screen.handle, &ri);

    XrVector3f fwd = QRot(g_ref.orientation, { 0, 0, -kScreenDistance });
    layer.space = g_localSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = g_screen.handle;
    layer.subImage.imageRect.offset = { 0, 0 };
    layer.subImage.imageRect.extent = { (int32_t)g_screenW, (int32_t)g_screenH };
    layer.pose.orientation = g_ref.orientation;
    layer.pose.position = { g_ref.position.x + fwd.x, g_ref.position.y + fwd.y, g_ref.position.z + fwd.z };
    layer.size = { kScreenWidth, kScreenWidth * g_screenH / g_screenW };
    return true;
}

static void RunFrame(IDXGISwapChain* swapChain, int renderedEye)
{
    XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs{ XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame_(g_session, &fwi, &fs))) return;
    XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (XR_FAILED(xrBeginFrame_(g_session, &fbi))) return;
    PollControllers(fs.predictedDisplayTime);

    // Frame pacing: a frame is "late" if more than 1.5 display periods passed
    // since the previous one (SteamVR then reprojects/motion-smooths, which can
    // look like wobbly 'water' distortion).
    {
        static LARGE_INTEGER freq = {}, last = {};
        static int frames = 0, late = 0;
        static double worstMs = 0, sumMs = 0;
        static DWORD lastLog = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        if (last.QuadPart)
        {
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            double periodMs = fs.predictedDisplayPeriod / 1e6;
            frames++; sumMs += ms;
            if (ms > worstMs) worstMs = ms;
            if (periodMs > 0 && ms > 1.5 * periodMs) late++;
        }
        last = now;
        if (GetTickCount() - lastLog > 5000 && frames)
        {
            lastLog = GetTickCount();
            Log("[xr] pacing: %d frames, avg %.2f ms, worst %.2f ms, late %d (%.1f%%), display period %.2f ms",
                frames, sumMs / frames, worstMs, late, 100.0 * late / frames, fs.predictedDisplayPeriod / 1e6);
            frames = late = 0; worstMs = sumMs = 0;
        }
    }

    XrCompositionLayerProjectionView projViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader* layers[1] = { (XrCompositionLayerBaseHeader*)&layer };
    uint32_t layerCount = 0;

    XrCompositionLayerQuad screenLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerDepthInfoKHR depthInfo[2] = { { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR }, { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR } };
    float xs, ys;
    // Full VR while the game shows a 3D camera; otherwise (title, menu, intro
    // video) show its image on a floating virtual screen.
    if (fs.shouldRender && StereoProjectionFresh() && StereoProjection(xs, ys))
    {
        ID3D11Texture2D* bb = nullptr;
        swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        D3D11_TEXTURE2D_DESC desc = {};
        bb->GetDesc(&desc);

        // 1. The frame just finished was rendered with the poses we handed
        //    out last time: copy it to its eye and remember that pose.
        ID3D11DeviceContext* ctx = nullptr;
        g_device->GetImmediateContext(&ctx);
        ID3D11Texture2D* src = bb;
        if (g_resolve)
        {
            ctx->ResolveSubresource(g_resolve, 0, bb, 0, desc.Format);
            src = g_resolve;
        }
        if (renderedEye == 2)
        {
            // Double render: left eye in the backbuffer, right eye in its twin.
            ID3D11Resource* twin = ShadowOfResource(bb);
            ID3D11Texture2D* twinSrc = (ID3D11Texture2D*)twin;
            if (twin && g_resolve)
            {
                ctx->ResolveSubresource(g_resolveRight ? g_resolveRight : g_resolve, 0, twin, 0, desc.Format);
                twinSrc = g_resolveRight ? g_resolveRight : g_resolve;
            }
            CopyToEye(0, ctx, src, desc);
            if (twinSrc) CopyToEye(1, ctx, twinSrc, desc);
            if (g_depthReady)
            {
                ID3D11Resource* depthL = CaptureSceneDepth();
                ID3D11Resource* depthR = depthL ? ShadowOfResource(depthL) : nullptr;
                WriteDepth(0, ctx, depthL);
                WriteDepth(1, ctx, depthR);
                if (depthR) depthR->Release();
                if (depthL) depthL->Release();
            }
            if (twin) twin->Release();
            if (g_givenValid && twinSrc)
            {
                g_imagePose[0] = g_givenPose[0]; g_imagePose[1] = g_givenPose[1];
                g_imagePoseValid[0] = g_imagePoseValid[1] = true;
            }
        }
        else
        for (int e = 0; e < 2; ++e)
        {
            if ((e == 0 && renderedEye > 0) || (e == 1 && renderedEye < 0)) continue;
            CopyToEye(e, ctx, src, desc);
            if (g_givenValid)
            {
                // Mono frames were rendered from the left eye's pose.
                g_imagePose[e] = g_givenPose[renderedEye == 0 ? 0 : e];
                g_imagePoseValid[e] = true;
            }
        }
        ctx->Release();
        bb->Release();

        // 2. Predict where the eyes will be when the *next* frame shows, and
        //    hand that to the game (relative to the recentered reference).
        XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime + fs.predictedDisplayPeriod;
        li.space = g_localSpace;
        XrViewState vs{ XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t vc = 0;
        bool located = XR_SUCCEEDED(xrLocateViews_(g_session, &li, &vs, 2, &vc, views)) && vc == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

        XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
        bool headOk = XR_SUCCEEDED(xrLocateSpace_(g_viewSpace, g_localSpace, li.displayTime, &head)) &&
            (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (headOk && (!g_refSet || KeyEdge(VK_F6) || g_recenterPending))
        {
            Recenter(head.pose);
            g_recenterPending = false;
        }

        // Live comfort tuning: [ ] back/forward, , . down/up (0.25 m steps).
        {
            float back = g_camBack, up = g_camUp;
            if (KeyEdge(VK_OEM_4)) back += 0.25f;      // [
            if (KeyEdge(VK_OEM_6)) back -= 0.25f;      // ]
            if (KeyEdge(VK_OEM_COMMA)) up -= 0.25f;    // ,
            if (KeyEdge(VK_OEM_PERIOD)) up += 0.25f;   // .
            if (back != g_camBack || up != g_camUp)
            {
                g_camBack = back; g_camUp = up;
                wchar_t v[32];
                swprintf_s(v, L"%.2f", back); WritePrivateProfileStringW(L"xr", L"cameraBack", v, g_iniPath);
                swprintf_s(v, L"%.2f", up); WritePrivateProfileStringW(L"xr", L"cameraUp", v, g_iniPath);
                Log("[xr] camera offset: back %.2f, up %.2f game units (saved)", back, up);
            }
        }

        if (located && g_refSet)
        {
            // Camera offsets are game distances: pre-divide by worldScale, which
            // the stereo code multiplies every pose by (so F10/F11 don't shrink them).
            float ws = Stereo().worldScale;
            float camToMeters = 1.0f / (ws > 1e-4f ? ws : 1e-4f);
            XrQuaternionf refInv = QConj(g_ref.orientation);
            for (int e = 0; e < 2; ++e)
            {
                StereoSetDisplayFov(e, tanf(views[e].fov.angleLeft), tanf(views[e].fov.angleRight),
                    tanf(views[e].fov.angleUp), tanf(views[e].fov.angleDown),
                    (float)g_size / desc.Width, (float)g_size / desc.Height);

                XrQuaternionf q = QMul(refInv, views[e].pose.orientation);
                XrVector3f d = { views[e].pose.position.x - g_ref.position.x,
                                 views[e].pose.position.y - g_ref.position.y,
                                 views[e].pose.position.z - g_ref.position.z };
                XrVector3f p = QRot(refInv, d);
                float rot[9], pos[3] = { p.x, p.y + g_camUp * camToMeters, p.z + g_camBack * camToMeters };
                QToMat(q, rot);
                StereoSetEyePose(e, rot, pos);
                g_givenPose[e] = views[e].pose;
                g_eyeFov[e] = views[e].fov;
            }
            g_givenValid = true;

            if (headOk)
            {
                XrQuaternionf q = QMul(refInv, head.pose.orientation);
                XrVector3f d = { head.pose.position.x - g_ref.position.x,
                                 head.pose.position.y - g_ref.position.y,
                                 head.pose.position.z - g_ref.position.z };
                XrVector3f p = QRot(refInv, d);
                float rot[9], pos[3] = { p.x, p.y + g_camUp * camToMeters, p.z + g_camBack * camToMeters };
                QToMat(q, rot);
                StereoSetHeadPose(rot, pos);
            }
        }

        // 3. Submit both eyes with the exact pose each image was rendered with.
        if (g_imagePoseValid[0] && g_imagePoseValid[1] && g_eyes[0].everReleased && g_eyes[1].everReleased)
        {
            for (int e = 0; e < 2; ++e)
            {
                projViews[e].pose = g_imagePose[e];
                projViews[e].fov = g_eyeFov[e];
                // Depth (game units -> meters: divide by worldScale).
                float nearU, farU;
                if (g_depthReady && g_depthWritten[0] && g_depthWritten[1] && StereoDepthRange(nearU, farU))
                {
                    float ws = Stereo().worldScale > 1e-4f ? Stereo().worldScale : 1e-4f;
                    depthInfo[e] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                    depthInfo[e].subImage.swapchain = g_depthSc[e].handle;
                    depthInfo[e].subImage.imageRect.offset = { 0, 0 };
                    depthInfo[e].subImage.imageRect.extent = { (int32_t)g_size, (int32_t)g_size };
                    depthInfo[e].minDepth = 0.0f;
                    depthInfo[e].maxDepth = 1.0f;
                    depthInfo[e].nearZ = nearU / ws;
                    depthInfo[e].farZ = farU / ws;
                    projViews[e].next = &depthInfo[e];
                    static bool logged = false;
                    if (!logged) { logged = true; Log("[xr] submitting depth: near %.3f m, far %.1f m", depthInfo[e].nearZ, depthInfo[e].farZ); }
                }
                projViews[e].subImage.swapchain = g_eyes[e].handle;
                projViews[e].subImage.imageRect.offset = { 0, 0 };
                projViews[e].subImage.imageRect.extent = { (int32_t)g_size, (int32_t)g_size };
            }
            layer.space = g_localSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            layerCount = 1;

            if (g_submitted++ % 900 == 0)
                Log("[xr] submitting: eye fov L%.1f R%.1f U%.1f D%.1f deg, game xs=%.3f ys=%.3f, eye=%d, period %.2f ms",
                    g_eyeFov[0].angleLeft * 57.2958f, g_eyeFov[0].angleRight * 57.2958f,
                    g_eyeFov[0].angleUp * 57.2958f, g_eyeFov[0].angleDown * 57.2958f,
                    xs, ys, renderedEye, fs.predictedDisplayPeriod / 1e6);
        }
    }
    else if (fs.shouldRender && SubmitScreen(swapChain, fs, screenLayer))
    {
        layers[0] = (XrCompositionLayerBaseHeader*)&screenLayer;
        layerCount = 1;
    }
    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    XrResult r = xrEndFrame_(g_session, &fei);
    if (XR_FAILED(r))
    {
        static int errs = 0;
        if (errs++ < 10) Log("[xr] xrEndFrame failed: %s", ResultStr(r));
    }
}
void XrSubmitFrame(IDXGISwapChain* swapChain, int renderedEye)
{
    switch (g_stage)
    {
    case XrStage::Off:
    case XrStage::Failed:
        return;
    case XrStage::NeedInstance:
        if (!xrGetInstanceProcAddr_ && !LoadLoader()) { g_stage = XrStage::Failed; return; }
        if (GetTickCount() < g_nextRetry) return;
        g_nextRetry = GetTickCount() + 5000;
        if (!CreateInstance()) return;
        g_nextRetry = 0;
        g_stage = XrStage::NeedSystem;
        // fallthrough
    case XrStage::NeedSystem:
        if (GetTickCount() < g_nextRetry) return;
        g_nextRetry = GetTickCount() + 2000;
        if (!GetSystem()) return;
        if (!CreateSession(swapChain)) { g_stage = XrStage::Failed; return; }
        g_stage = XrStage::Running;
        // fallthrough
    case XrStage::Running:
        PollEvents();
        if (g_sessionRunning && g_stage == XrStage::Running)
            RunFrame(swapChain, renderedEye);
        return;
    }
}

bool XrSessionActive() { return g_sessionRunning; }
