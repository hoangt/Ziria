# 
# Copyright (c) Microsoft Corporation
# All rights reserved. 
#
# Licensed under the Apache License, Version 2.0 (the ""License""); you
# may not use this file except in compliance with the License. You may
# obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
# LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
# A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
#
# See the Apache Version 2.0 License for specific language governing
# permissions and limitations under the License.
#
#

#!/bin/bash

# Ziria compilation script for MAC example
# Since there are multiple Ziria files, 
# we first compile each using this script
# and then link everything together using ccompile-mac.

set -e

export TOP=$(cd $(dirname $0)/.. && pwd -P)
export ARCH=$(uname -p)
source $TOP/scripts/common.sh

echo $1
echo `pwd`
#echo "Preprocessing..."
#gcc -x c -P -E $1 >$1.expanded
gcc $DEFINES -I $TOP/lib -w -x c -E $1 >$2.expanded


#echo "Running WPL compiler..."
$WPLC $WPLCFLAGS $EXTRAOPTS -i $2.expanded -o $2.c
if [ "$ZIRIA_TARGET_ARCH" = "" ]
then
	mv $2.c $TOP/csrc/mac/$2.c
else
    scp $2.c $ZIRIA_TARGET_USER@$ZIRIA_TARGET_ADDR:$ZIRIA_TARGET_PATH
fi



