#!/usr/bin/env bash
set -e

# reset coverage data and generate gcc note file
scons -j$(nproc) -D --coverage

# run safety tests to generate coverage data
#./test.sh
#./test_toyota.py
#./test_ford.py TestFordStockSafety.test_rx_hook
#./test_honda.py # TestHyundaiCanfdHDA2LongEV.test_rx_hook
#./test_defaults.py
./test_body.py || true

# generate and open report
if [ "$1" == "--report" ]; then
  geninfo ../libpanda/ -o coverage.info
  genhtml coverage.info -o coverage-out
#  browse coverage-out/index.html
fi

# test coverage
GCOV_OUTPUT=$(gcov -n ../libpanda/panda.c)
INCOMPLETE_COVERAGE=$(echo "$GCOV_OUTPUT" | paste -s -d' \n' | grep "File.*safety/safety_.*.h" | grep -v "100.00%")
if [ -n "$INCOMPLETE_COVERAGE" ]; then
  echo "Some files have less than 100% coverage:"
  echo "$INCOMPLETE_COVERAGE"
  exit 1
else
  echo "All checked files have 100% coverage!"
fi
