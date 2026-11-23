"""Run the portable C firmware suites; no RF, BLE, SDK or miniapp dependency."""
import argparse, json, pathlib, subprocess, tempfile, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default='gcc', help='C compiler executable path (gcc, clang or tcc)')
a = p.parse_args()
deps = json.loads((ROOT/'tools/core-tests.json').read_text())
found = {f.stem.removesuffix('_host') for f in (ROOT/'tests').glob('*_host.c')}
if found != set(deps): p.error('Test manifest drift')
core = ROOT/'firmware/c3_controller/main'
failed = []
with tempfile.TemporaryDirectory(prefix='c5-ctf-tests-') as temporary:
 for name, sources in deps.items():
  output = pathlib.Path(temporary)/(name+'.exe')
  command = [a.cc, '-std=c11', *[str(core/(s+'.c')) for s in sources], str(ROOT/'tests'/(name+'_host.c')), '-o', str(output)]
  try:
   build = subprocess.run(command, capture_output=True, text=True, timeout=60)
   if build.returncode: raise RuntimeError(build.stdout+build.stderr)
   run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
   if run.returncode: raise RuntimeError(run.stdout+run.stderr)
   print('PASS '+name, flush=True)
  except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
   failed.append(name); print('FAIL '+name+': '+str(error), flush=True)
print(f'{len(deps)-len(failed)}/{len(deps)} suites passed')
sys.exit(bool(failed))
