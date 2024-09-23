#!/usr/bin/env bash
set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
PANDA_DIR=$(realpath $DIR/../../)

GREEN="\e[1;32m"
YELLOW="\e[1;33m"
RED="\e[1;31m"
NC='\033[0m'

: "${CPPCHECK_DIR:=$DIR/cppcheck/}"

# install cppcheck if missing
if [ -z "${SKIP_CPPCHECK_INSTALL}" ]; then
  $DIR/install.sh
fi

# ensure checked in coverage table is up to date
cd $DIR
if [ -z "$SKIP_TABLES_DIFF" ]; then
  python3 $CPPCHECK_DIR/addons/misra.py -generate-table > coverage_table
  if ! git diff --quiet coverage_table; then
    echo -e "${YELLOW}MISRA coverage table doesn't match. Update and commit:${NC}"
    exit 3
  fi
fi

cd $PANDA_DIR
if [ -z "${SKIP_BUILD}" ]; then
  scons -j8 --compile_db
fi

CHECKLIST=$DIR/checkers.txt
echo "Cppcheck checkers list from test_misra.sh:" > $CHECKLIST

cppcheck() {
  # get all gcc defines: arm-none-eabi-gcc -dM -E - < /dev/null
  COMMON_DEFINES="-D__GNUC__=9"

  # note that cppcheck build cache results in inconsistent results as of v2.13.0
  OUTPUT=$DIR/.output.log

  echo -e "\n\n\n\n\nTEST variant options:" >> $CHECKLIST
  echo -e ""${@//$PANDA_DIR/}"\n\n" >> $CHECKLIST # (absolute path removed)

  $CPPCHECK_DIR/cppcheck --inline-suppr -I $PANDA_DIR/board/ \
          --library=gnu.cfg --suppress=missingIncludeSystem \
          --suppressions-list=$DIR/suppressions.txt --suppress=*:*inc/* \
          --error-exitcode=2 --check-level=exhaustive --safety \
          --platform=arm32-wchar_t4 $COMMON_DEFINES --checkers-report=$CHECKLIST.tmp \
          --std=c11 "$@" |& tee $OUTPUT

  cat $CHECKLIST.tmp >> $CHECKLIST
  rm $CHECKLIST.tmp
  # cppcheck bug: some MISRA errors won't result in the error exit code,
  # so check the output (https://trac.cppcheck.net/ticket/12440#no1)
  if grep -e "misra violation" -e "error" -e "style: " $OUTPUT > /dev/null; then
    printf "${RED}** FAILED: MISRA violations found!${NC}\n"
    exit 1
  fi
}

if [ -z "$PANDA_SYSTEM_LEVEL_ONLY" ]; then
# Test whole Panda project for unused functions / constants
printf "\n${GREEN}** Panda project - tests: Cppcheck whole program + MISRA per translation unit **${NC}\n"
cppcheck  --project=$PANDA_DIR/compile_commands.json \
          --enable=all --addon=misra \
          -i$PANDA_DIR/board/bootstub.c \
          -i$PANDA_DIR/board/jungle/ \
          -i$PANDA_DIR/crypto/
# TODO enable bootstub and jungle for the whole build
# Note: MISRA system level tests (e.g. rule 2.5) not supported by cppcheck 2.15 with --project argument


# Test standalone Panda program. (Unused functions / constants will be more strict than whole project check)
  printf "\n${GREEN}** Panda standalone - tests: Cppcheck whole program **${NC}\n"
  cppcheck  --project=$PANDA_DIR/compile_commands_panda.json \
            --enable=all --addon=misra
fi

# *** Legacy cppcheck test type by providing c files list ***
# --force checks different macro combinations to avoid reporting macros and functions used by only one panda configuration
# panda_macro_config.h lets cppcheck know macro combinations used for the build
printf "\n${GREEN}** Panda tests: cppcheck whole program anlysis + Misra addon system level analysis **${NC}\n"
UNDEBUG="-UDEBUG -UDEBUG_COMMS -UDEBUG_FAULTS -UDEBUG_FAN -UDEBUG_SPI -UDEBUG_UART -UDEBUG_USB"
cppcheck  $PANDA_DIR/board/main.c -I$PANDA_DIR/board/ \
          -DPANDA -DALLOW_DEBUG \
          -UPANDA_JUNGLE -UFINAL_PROVISIONING -UBOOTSTUB $UNDEBUG \
          --config-exclude=$PANDA_DIR/board/stm32f4/inc --config-exclude=$PANDA_DIR/board/stm32h7/inc \
          --include=$DIR/panda_macro_config.h -i$DIR \
          --enable=all --addon=misra \
          --force
# TODO remove this test when cppcheck fully supports system level misra reporting with --project
printf "\n${GREEN}Success!${NC} took $SECONDS seconds\n"


# ensure list of checkers is up to date
cd $DIR
if [ -z "$SKIP_TABLES_DIFF" ] && ! git diff --quiet $CHECKLIST; then
  echo -e "\n${YELLOW}WARNING: Cppcheck checkers.txt report has changed. Review and commit...${NC}"
  exit 4
fi
