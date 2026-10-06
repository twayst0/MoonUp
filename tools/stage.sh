#!/bin/bash
# Copies the distributable into /mnt/user-data/outputs/MoonUp and prints a commit list (JSON)
# containing only files changed since the previous stage.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/mnt/user-data/outputs/MoonUp
STAMP=$ROOT/build/.last_stage
mkdir -p $OUT
cp -u $ROOT/build/MoonUp.exe $ROOT/redist/WebView2Loader.dll $OUT/
[ -f $ROOT/README.md ] && cp -u $ROOT/README.md $OUT/
rsync -a --delete $ROOT/ui/ $OUT/ui/
cd /mnt/user-data/outputs
if [ -f $STAMP ] && [ "$1" != "all" ]; then FILES=$(find MoonUp -type f -newer $STAMP); else FILES=$(find MoonUp -type f); fi
touch $STAMP
echo "$FILES" | python3 -c "
import sys,json
f=[l.strip() for l in sys.stdin if l.strip()]
print(json.dumps([{'stagedPath':'/mnt/user-data/outputs/'+x,'devicePath':'Z:\\\\uygulama 4\\\\'+x.replace('/','\\\\')} for x in f]))"
