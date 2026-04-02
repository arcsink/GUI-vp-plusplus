#!/bin/bash

set -e

echo "Post-Build: begin"

echo " + make udev and Xorg start up optional"
mkdir -p $TARGET_DIR/etc/init.d/optional_xorg
for file in \
	$TARGET_DIR/etc/init.d/S??udev \
	$TARGET_DIR/etc/init.d/S??xorg \
	; do
	[[ -e $file ]] && mv $file $TARGET_DIR/etc/init.d/optional_xorg
done

echo " + build guivp_pbmt_test helper"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
PBMT_HELPER_SRC="${ROOT_DIR}/tools/guivp_pbmt_test.c"
PBMT_HELPER_DST="${TARGET_DIR}/usr/bin/guivp_pbmt_test"
PBMT_HELPER_CC="${TARGET_CC}"

if [[ -z "${PBMT_HELPER_CC}" && -n "${HOST_DIR}" ]]; then
	for candidate in "${HOST_DIR}"/bin/*-linux-gnu-gcc; do
		if [[ -x "${candidate}" ]]; then
			PBMT_HELPER_CC="${candidate}"
			break
		fi
	done
fi

if [[ -z "${PBMT_HELPER_CC}" || ! -x "${PBMT_HELPER_CC}" ]]; then
	echo "Post-Build: unable to find cross compiler for guivp_pbmt_test" >&2
	exit 1
fi

mkdir -p "$(dirname "${PBMT_HELPER_DST}")"
"${PBMT_HELPER_CC}" -O2 -Wall -Wextra -std=c11 "${PBMT_HELPER_SRC}" -o "${PBMT_HELPER_DST}"

echo "Post-Build: done"
