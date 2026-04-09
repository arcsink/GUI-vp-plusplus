#!/bin/bash

git remote add public https://github.com/google/googletest.git
git pull public master # Creates a merge commit
git push origin master
