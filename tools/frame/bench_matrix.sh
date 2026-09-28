#!/bin/bash
# Benchmark several settings on the Frame (headset on the table): each line is
#   size msaa density distance effects aniso
# e.g.  ./bench_matrix.sh "1440 1 Medium Low Low 16" "1440 4 Medium Low Low 16"
# Prints GPU/CPU averages and worst frames (last ~20 s of each run) and late frames.
G=~/Games/Flower_GOG; C=$G/vrmod_Flower.cfg
cp $C ~/Games/cfg.before_matrix; cp $G/vrmod.ini ~/Games/ini.before_matrix
for cfg in "$@"; do
  set -- $cfg
  size=$1; msaa=$2; dens=$3; dist=$4; eff=$5; aniso=$6
  sed -i -E "s/<Screen [^>]*>/<Screen Anisotrophy=\"$aniso\" FullScreen=\"false\" Height=\"$size\" MultiSampleCount=\"$msaa\" VSync=\"true\" Width=\"$size\"\/>/" $C
  sed -i -E "s/<Density Setting=\"[A-Za-z]+\">/<Density Setting=\"$dens\">/; s/<Distance Setting=\"[A-Za-z]+\">/<Distance Setting=\"$dist\">/; s/<Effects Setting=\"[A-Za-z]+\">/<Effects Setting=\"$eff\">/" $C
  sed -i -E "s/^maxSquare=.*/maxSquare=$size/" $G/vrmod.ini
  ~/Games/level1_bench.sh > /tmp/bench.out 2>&1
  inlevel=$(grep -c IN-LEVEL /tmp/bench.out)
  bb=$(grep -o "backbuffer [0-9x]* fmt=[0-9]* samples=[0-9]*" $G/vrmod.log | head -1)
  grep -F "[perf]" $G/vrmod.log | tail -4 | awk -v c="$cfg" -v bb="$bb" -v il="$inlevel" '
    { for (i=1;i<=NF;i++) { if ($i=="gpu") { g+=$(i+2); if ($(i+5)>gw) gw=$(i+5) } if ($i=="cpu") { c2+=$(i+2); if ($(i+5)>cw) cw=$(i+5) } } n++ }
    END { if (n) printf "%-26s | %s | in-level %s | gpu %.2f ms (worst %.1f) | cpu %.2f ms (worst %.1f)", c, bb, il, g/n, gw, c2/n, cw }'
  grep -F "[xr] pacing" $G/vrmod.log | tail -4 | awk '{ for (i=1;i<=NF;i++) if ($i=="late") { l+=$(i+1) } if ($3 ~ /^[0-9]+$/) f+=$3 } END { if (f) printf " | late %.1f%%\n", 100*l/f; else print "" }'
done
pkill -f "[F]lower\.exe"
