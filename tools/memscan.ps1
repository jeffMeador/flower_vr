# Scan the running Flower process for camera-like float groups: a far clip
# value (default 1330.5) with the near clip (0.1) and aspect within +/-32
# bytes. Prints address and the surrounding floats.
param([float]$Far = 1330.5, [float]$Near = 0.1, [float]$Aspect = 3.5555556)

Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class MemScan {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int a, bool b, int pid);
  [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
  [DllImport("kernel32.dll")] static extern int VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, int len);
  [StructLayout(LayoutKind.Sequential)] public struct MBI { public IntPtr Base, AllocBase; public uint AllocProtect; public IntPtr Size; public uint State, Protect, Type; }
  static bool Near(float a, float b, float tol) { return Math.Abs(a - b) <= tol; }
  public static List<string> Scan(int pid, float far, float near, float aspect) {
    var res = new List<string>();
    IntPtr h = OpenProcess(0x0410, false, pid);
    long addr = 0; MBI m;
    while (VirtualQueryEx(h, (IntPtr)addr, out m, Marshal.SizeOf(typeof(MBI))) != 0) {
      long size = (long)m.Size;
      bool readable = m.State == 0x1000 && (m.Protect & 0x04) != 0 && (m.Protect & 0x100) == 0; // committed RW, not guard
      if (readable && size < (1L << 31)) {
        var buf = new byte[size]; IntPtr read;
        if (ReadProcessMemory(h, m.Base, buf, (IntPtr)size, out read)) {
          int n = (int)read;
          for (int i = 0; i + 4 <= n; i += 4) {
            float f = BitConverter.ToSingle(buf, i);
            if (f != far) continue;
            bool hasNear = false, hasAspect = false;
            for (int j = Math.Max(0, i - 32); j + 4 <= Math.Min(n, i + 36); j += 4) {
              float g = BitConverter.ToSingle(buf, j);
              if (Near(g, near, 1e-6f)) hasNear = true;
              if (Near(g, aspect, 1e-3f)) hasAspect = true;
            }
            if (!hasNear) continue;
            var s = new System.Text.StringBuilder();
            s.AppendFormat("{0:X} aspect={1} :", (long)m.Base + i, hasAspect);
            for (int j = Math.Max(0, i - 32); j + 4 <= Math.Min(n, i + 36); j += 4)
              s.AppendFormat(" {0}{1:G5}", j == i ? "*" : "", BitConverter.ToSingle(buf, j));
            res.Add(s.ToString());
          }
        }
      }
      addr = (long)m.Base + size;
    }
    return res;
  }
}
"@
$p = Get-Process Flower | Select-Object -First 1
[MemScan]::Scan($p.Id, $Far, $Near, $Aspect)
