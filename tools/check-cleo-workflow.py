"""Validate actual CLEO sources with the reviewed Dryxio checkout."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

PIN = '35e60c3f7037e39dcb73d415dca16e657125f398'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cleo-ai', required=True, type=Path)
parser.add_argument('--sanny-root', type=Path)
args = parser.parse_args()
upstream = args.cleo_ai.resolve()
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=upstream, text=True).strip()
if revision != PIN:
    raise SystemExit('CLEO AI revision differs from the reviewed pin: ' + revision)
profile = json.loads((upstream/'config/target-profile.json').read_text())
if (profile['game_version'], profile['cleo']['version'], profile['sanny_builder']['version']) != ('1.0', '5.4.0', '4.2.0'):
    raise SystemExit('Unexpected CLEO target profile.')
if not (upstream/'source_data/sa.json').is_file():
    raise SystemExit('Run tools/sync_reference.py in the CLEO AI checkout first.')
root = Path(__file__).resolve().parents[1]
sources = []
source_root = root/'client' if (root/'client').is_dir() else root/'valkyrie-asi-suite'
for path in source_root.rglob('*'):
    if path.suffix.lower() not in {'.txt', '.sc', '.cs', '.cm', '.s'}:
        continue
    if any(part in {'third-party', 'third_party', 'build', 'res'} for part in path.parts):
        continue
    if path.stat().st_size > 2_000_000:
        continue
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    if re.search(r'\{\$CLEO\b', text, re.I):
        sources.append(path)
if not sources:
    print('CLEO: not applicable (0 CLEO script sources). Native ASIs require MSVC and their native tests.')
    raise SystemExit(0)
subprocess.run([sys.executable, str(upstream/'tools/validate_workspace.py'), '--strict', *map(str, sources)], check=True)
if not args.sanny_root:
    raise SystemExit('Static validation passed; compilation is REQUIRED. Supply --sanny-root for Sanny Builder 4.2.0.')
sanny = args.sanny_root/'sanny.exe'
if not sanny.is_file():
    raise SystemExit('Missing sanny.exe.')
output = root/'work/cleo-compiled'
for source in sources:
    target = output/source.relative_to(root).with_suffix('.cm' if '{$CLEO .cm}' in source.read_text() else '.cs')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.unlink(missing_ok=True)
    subprocess.run([str(sanny), '--compile', str(source), str(target), '--no-splash', '--mode', 'sa_sbl', '-o', 'Compiler::CheckConditions', '1', '-o', 'Compiler::CheckLocalVariables', '1'], check=True)
    if not target.is_file():
        raise SystemExit('Sanny did not produce output for ' + str(source))
print(f'Validated and compiled {len(sources)} CLEO source(s); gameplay validation remains separate.')
