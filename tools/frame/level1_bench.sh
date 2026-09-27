#!/bin/bash
# Launch Flower on the Frame, fly into level 1 with the virtual pad, report frame timing.
# Needs [debug] fakepad=1 in vrmod.ini (the virtual pad reads vrmod_pad.txt). Copy to ~/Games on the Frame.
G=~/Games/Flower_GOG; P=$G/vrmod_pad.txt
pad(){ printf "%s" "$*" > $P; }
pkill -f "[F]lower\.exe"; sleep 3; pad 0 0; rm -f $G/vrmod.log
nohup ~/Games/run_flower.sh > ~/Games/run_flower.out 2>&1 < /dev/null & disown
sleep 20; pad 0 0 A; sleep 0.4; pad 0 0; sleep 12
for t in 1 2 3 4 5 6; do
  grep -q "proj from VP" $G/vrmod.log && break
  pad -0.7 0; sleep 1.2; pad 0 0 RT; sleep 5; pad 0 0
  for w in $(seq 1 12); do sleep 5; grep -q "proj from VP" $G/vrmod.log && break; done
done
grep -q "proj from VP" $G/vrmod.log && echo IN-LEVEL || echo NOT-IN-LEVEL
pad 0.3 0 RT; sleep 30; pad 0 0
grep -E "backbuffer" $G/vrmod.log; grep -E 'pacing|\[perf\]' $G/vrmod.log | tail -4
