#!/bin/bash
for i in {0..200}; do
    random=$((RANDOM % (15 - 1 + 1) + 1))
    ./client 127.0.0.1 10000 /files/test$random.html &
done