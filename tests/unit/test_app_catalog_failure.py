#!/usr/bin/env python3
"""Generate a fixture from real app-load control flow and migration functions.

Per-app field conversion, profile policy, and file I/O are test boundaries; the
real process_catalog publication code is linked by the CMake target.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.root.resolve()
source = (root / 'src/process.cpp').read_text()
migrate_start = source.index('  void migrate_apps(')
migrate_end = source.index('  std::optional<proc::proc_t> parse(', migrate_start)
load_start = source.index('    bool apps_parsed_ok = false;', migrate_end)
load_end = source.index('    // Virtual Display entry', load_start)
loop = source[load_start:load_end]
app_start = loop.index('        for (auto &app_node : tree["apps"]) {')
app_end = loop.index('        fail_count = 0;', app_start)
loop = loop[:app_start] + '''        // App field conversion is a deterministic fixture boundary. Real JSON
        // loading, migration retries, failure guard and publication are used.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t app {};
          app.uuid = app_node.at("uuid").get<std::string>();
          app.name = app_node.at("name").get<std::string>();
          app.id = std::to_string(++i);
          apps.emplace_back(std::move(app));
        }

''' + loop[app_end:]
template = Path(__file__).with_suffix('.cpp.in').read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(template.replace('@MIGRATION@', source[migrate_start:migrate_end]).replace('@LOAD_LOOP@', loop))
