"""Makes every float literal in a generated weights header valid C++ (e.g. '1f' -> '1.0f')."""
import re, sys
for p in sys.argv[1:]:
    s = open(p).read()
    s = re.sub(r'(?<=[{,])(-?\d+)f', r'\1.0f', s)
    open(p, 'w').write(s)
