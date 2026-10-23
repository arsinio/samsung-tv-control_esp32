#!/usr/bin/env bash
# Run idf.py with ESP-IDF v5.2 and its Python 3.9 env, e.g. ./idf.sh build flash monitor
set -euo pipefail
export IDF_PATH="${IDF_PATH:-$HOME/esp/v5.2/esp-idf}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.2_py3.9_env}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
cd "$(dirname "$0")"
exec idf.py "$@"
