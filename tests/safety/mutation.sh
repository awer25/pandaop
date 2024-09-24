#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
cd $DIR

cd ../../
scons --mutation -j$(nproc)
cd $DIR

SAFETY_MODELS=( body chrysler defaults gm honda hyundai_canfd hyundai mazda nissan )
#SAFETY_MODELS=( volkswagen_pq )
for safety_model in "${SAFETY_MODELS[@]}"; do
  echo -e "\n\nTesting mutation on : safety_$safety_model"
  echo -e "" > mull.yml
  #echo -e "includePaths:\n - \".*safety_$safety_model.h\"" > mull.yml
  echo -e "gitDiffRef: master\n" >> mull.yml
  echo -e "gitProjectRoot: ../../" >> mull.yml

  #PYTHONPATH=/home/batman/:/home/batman/panda/opendbc/:$PYTHONPATH mull-runner-17 --ld-search-path /lib/x86_64-linux-gnu/ ../libpanda/libpanda.so -test-program=./test_mazda.py
  mull-runner-17 --ld-search-path /lib/x86_64-linux-gnu/ ../libpanda/libpanda.so -test-program=./test_$safety_model.py || true
done