"""Build public firmware using an already activated ESP-IDF environment."""
import argparse, os, pathlib, shutil, subprocess, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('project', choices=['c3_controller', 'c5_detector'])
p.add_argument('--build-dir', default='build-public')
a = p.parse_args()
idf = pathlib.Path(os.environ.get('IDF_PATH', '')) / 'tools/idf.py'
if not idf.is_file(): p.error('Activate ESP-IDF v6.0.2 first (IDF_PATH missing)')
project = ROOT / 'firmware' / a.project
if not (project/'sdkconfig').exists(): shutil.copyfile(project/'sdkconfig.release', project/'sdkconfig')
raise SystemExit(subprocess.call([sys.executable, str(idf), '-B', a.build_dir, 'build'], cwd=project))
