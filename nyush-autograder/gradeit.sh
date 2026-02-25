#!/bin/bash

echo -e "\e[1;33mPreparing test cases...\e[m"
if ! sha1sum -c --quiet checksum.txt; then
  echo -e "\e[1;31mSome test cases are corrupted. Please make sure you have not modified any files provided.\e[m"
  exit 1
fi

NYUSH_GRADING=$PWD/nyush-grading
rm -rf $NYUSH_GRADING
mkdir $NYUSH_GRADING
if ! unzip -d $NYUSH_GRADING *.zip 2> /dev/null; then
  echo -e "\e[1;31mThere was an error extracting your source code. Please make sure your zip file is in the current directory.\e[m"
  exit 1
fi

echo -e "\e[1;33mCompiling nyush...\e[m"
source scl_source enable gcc-toolset-12
if ! make -C $NYUSH_GRADING; then
  echo -e "\e[1;31mThere was an error compiling nyush. Please make sure your source code and Makefile are in the root of your submission.\e[m"
  exit 1
fi
if [ ! -f $NYUSH_GRADING/nyush ]; then
  echo -e "\e[1;31mThere was an error compiling nyush. Please make sure your Makefile generates an executable file named nyush.\e[m"
  exit 1
fi

echo -e "\e[1;33mRunning nyush...\e[m"
killall -q -9 nyush
rm -rf myoutputs
mkdir -p myoutputs $NYUSH_GRADING/subdir
UTILS=$PWD/`uname -m`
cp $UTILS/* $NYUSH_GRADING
cp $UTILS/print_numbers $NYUSH_GRADING/subdir

preprocess() {
cat << EOF > input.txt
Hello, World
Hello
World
Hello
Lorem Ipsum

This is the end of the input.txt file
EOF

rm -f output*.txt
rm -f append*.txt

cat << EOF > append.txt
Appending text to
file
in the command
EOF

cp append.txt append2.txt
cp append.txt append3.txt
}

case0="-n 22"
for i in {0..60}; do
  if [ $i -eq 0 ]; then
    echo -e "\e[1;33mTesting prompt...\e[m"
  elif [ $i -eq 1 ]; then
    echo -e "\e[1;33mTesting process creation and termination...\e[m"
  elif [ $i -eq 21 ]; then
    echo -e "\e[1;33mTesting I/O redirection...\e[m"
  elif [ $i -eq 31 ]; then
    echo -e "\e[1;33mTesting pipes...\e[m"
  elif [ $i -eq 41 ]; then
    echo -e "\e[1;33mTesting jobs and fg...\e[m"
  elif [ $i -eq 51 ]; then
    echo -e "\e[1;33mTesting cd and error handling...\e[m"
  fi
  cd $NYUSH_GRADING
  preprocess
  $(timeout 2 bash -c "env LD_PRELOAD=$NYUSH_GRADING/libnosystem.so ./nyush < ../inputs/input$i > ../myoutputs/output$i 2> ../myoutputs/stderr$i")
  status=$?
  cd ..
  if [ $status -eq 202 ]; then
    echo -e "\e[1;31mCase $i FAILED (system() not permitted)\e[m"
  elif [ $status -eq 124 ]; then
    echo -e "\e[1;31mCase $i FAILED (time limit exceeded)\e[m"
  elif [ $status -ne 0 ]; then
    echo -e "\e[1;31mCase $i FAILED (crashed)\e[m"
  elif [ ! -f myoutputs/output$i ]; then
    echo -e "\e[1;31mCase $i FAILED (missing output)\e[m"
  elif [ ! -f myoutputs/stderr$i ]; then
    echo -e "\e[1;31mCase $i FAILED (missing stderr)\e[m"
  elif [ $i -eq 20 ] && grep -q "<defunct>" myoutputs/output$i || [ $i -ne 20 ] && ! cmp $case0 -s refoutputs/output$i myoutputs/output$i > /dev/null; then
    if [ $i -eq 0 ]; then
      echo -e "\e[1;31mCase $i FAILED (wrong prompt)\e[m"
    elif [ $i -eq 20 ]; then
      echo -e "\e[1;31mCase $i FAILED (zombie detected)\e[m"
    else
      echo -e "\e[1;31mCase $i FAILED (wrong output)\e[m"
    fi
  elif ! cmp -s refoutputs/stderr$i myoutputs/stderr$i > /dev/null; then
    echo -e "\e[1;31mCase $i FAILED (wrong stderr)\e[m"
  elif killall -q -9 nyush; then
    echo -e "\e[1;31mCase $i FAILED (lingering process detected)\e[m"
  else
    echo -e "\e[1;32mCase $i PASSED\e[m"
  fi
  unset case0
done

echo -e "\e[1;33mCleaning up...\e[m"
rm -rf $NYUSH_GRADING
