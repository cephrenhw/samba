# Hey Emacs, this is a -*- shell-script -*- !!!  :-)

# Common variables and functions for all CTDB tests.

# Print a message and exit.
die ()
{
	echo "$1" >&2 ; exit ${2:-1}
}

# This expands the most probable problem cases like "." and "..".
TEST_SUBDIR=$(dirname "$0")
if [ $(dirname "$TEST_SUBDIR") = "." ] ; then
	TEST_SUBDIR=$(cd "$TEST_SUBDIR" ; pwd)
fi

# If we are running from within the source tree then, depending on the
# tests that we're running, we may need to add the top level bin/ and
# tools/ subdirectories to $PATH.  In this case, sanity check that
# run_tests.sh is in the expected place.  If the tests are installed
# then sanity check that TEST_BIN_DIR is set.
if $CTDB_TESTS_ARE_INSTALLED ; then
	if [ -z "$TEST_BIN_DIR" ] ; then
		die "CTDB_TESTS_ARE_INSTALLED but TEST_BIN_DIR not set"
	fi

	_test_bin_dir="$TEST_BIN_DIR"
else
	if [ ! -f "${CTDB_TEST_DIR}/run_tests.sh" ] ; then
		die "Tests not installed but can't find run_tests.sh"
	fi

	ctdb_dir=$(dirname "$CTDB_TEST_DIR")

	_tools_dir="${ctdb_dir}/tools"
	if [ -d "$_tools_dir" ] ; then
		PATH="${_tools_dir}:$PATH"
	fi

	_test_bin_dir="${ctdb_dir}/bin"
fi

case "$_test_bin_dir" in
/*) : ;;
*) _test_bin_dir="${PWD}/${_test_bin_dir}" ;;
esac
if [ -d "$_test_bin_dir" ] ; then
	PATH="${_test_bin_dir}:$PATH"
fi
