#!/usr/bin/env python3
# Copyright (c) 2020-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Script for verifying Bitcoin Core release binaries.

This script attempts to download the sum file SHA256SUMS and corresponding
signature file SHA256SUMS.asc from bitcoincore.org and bitcoin.org and
compares them.

The sum-signature file is signed by a number of builder keys. This script
ensures that there is a minimum threshold of signatures from pubkeys that
we trust. This trust is articulated on the basis of configuration options
here, but by default is based upon local GPG trust settings.

The builder keys are available in the guix.sigs repo:

    https://github.com/bitcoin-core/guix.sigs/tree/main/builder-keys

If a minimum good, trusted signature threshold is met on the sum file, we then
download the files specified in SHA256SUMS, and check if the hashes of these
files match those that are specified. The script returns 0 if everything passes
the checks. It returns 1 if either the signature check or the hash check
doesn't pass. If an error occurs the return value is >= 2.

Logging output goes to stderr and final binary verification data goes to stdout.

JSON output can by obtained by setting env BINVERIFY_JSON=1.
"""
import argparse
import difflib
import json
import logging
import os
import subprocess
import typing as t
import re
import sys
import shutil
import tempfile
import textwrap
import urllib.request
import enum
from hashlib import sha256
from pathlib import Path

# The primary host; this will fail if we can't retrieve files from here.
HOST1 = "https://bitcoincore.org"
HOST2 = "https://bitcoin.org"
VERSIONPREFIX = "bitcoin-core-"
SUMS_FILENAME = 'SHA256SUMS'
SIGNATUREFILENAME = f"{SUMS_FILENAME}.asc"


class ReturnCode(enum.IntEnum):
    SUCCESS = 0
    INTEGRITY_FAILURE = 1
    FILE_GET_FAILED = 4
    FILE_MISSING_FROM_ONE_HOST = 5
    FILES_NOT_EQUAL = 6
    NO_BINARIES_MATCH = 7
    NOT_ENOUGH_GOOD_SIGS = 9
    BINARY_DOWNLOAD_FAILED = 10
    BAD_VERSION = 11


def set_up_logger(is_verbose: bool = True) -> logging.Logger:
    """Set up a logger that writes to stderr."""
    log = logging.getLogger(__name__)
    log.setLevel(logging.INFO if is_verbose else logging.WARNING)
    console = logging.StreamHandler(sys.stderr)  # log to stderr
    console.setLevel(logging.DEBUG)
    formatter = logging.Formatter('[%(levelname)s] %(message)s')
    console.setFormatter(formatter)
    log.addHandler(console)
    return log


log = set_up_logger()


def indent(output: str) -> str:
    return textwrap.indent(output, '  ')


def bool_from_env(key, default=False) -> bool:
    if key not in os.environ:
        return default
    raw = os.environ[key]

    if raw.lower() in ('1', 'true'):
        return True
    elif raw.lower() in ('0', 'false'):
        return False
    raise ValueError(f"Unrecognized environment value {key}={raw!r}")


VERSION_FORMAT = "<major>.<minor>[.<patch>][-rc[0-9]][-platform]"
VERSION_EXAMPLE = "22.0-x86_64 or 0.21.0-rc2-osx"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    'version', type=str, help=(
        f'version of the bitcoin release to download; of the format '
        f'{VERSION_FORMAT}. Example: {VERSION_EXAMPLE}')
)
parser.add_argument(
    '-v', '--verbose', action='store_true',
    default=bool_from_env('BINVERIFY_VERBOSE'),
)
parser.add_argument(
    '-q', '--quiet', action='store_true',
    default=bool_from_env('BINVERIFY_QUIET'),
)
parser.add_argument(
    '--cleanup', action='store_true',
    default=bool_from_env('BINVERIFY_CLEANUP'),
    help='if specified, clean up files afterwards'
)
parser.add_argument(
    '--import-keys', action='store_true',
    default=bool_from_env('BINVERIFY_IMPORTKEYS'),
    help='if specified, ask to import each unknown builder key'
)
parser.add_argument(
    '--require-all-hosts', action='store_true',
    default=bool_from_env('BINVERIFY_REQUIRE_ALL_HOSTS'),
    help=(
        f'If set, require all hosts ({HOST1}, {HOST2}) to provide signatures. '
        '(Sometimes bitcoin.org lags behind bitcoincore.org.)')
)
parser.add_argument(
    '--min-good-sigs', type=int, action='store', nargs='?',
    default=int(os.environ.get('BINVERIFY_MIN_GOOD_SIGS', 3)),
    help=(
        'The minimum number of good signatures to require successful termination.'),
)
parser.add_argument(
    '--keyserver', action='store', nargs='?',
    default=os.environ.get('BINVERIFY_KEYSERVER', 'hkp://keyserver.ubuntu.com'),
    help='which keyserver to use',
)
parser.add_argument(
    '--trusted-keys', action='store', nargs='?',
    default=os.environ.get('BINVERIFY_TRUSTED_KEYS', ''),
    help='A list of trusted signer GPG keys, separated by commas. Not "trusted keys" in the GPG sense.',
)
parser.add_argument(
    '--json', action='store_true',
    default=bool_from_env('BINVERIFY_JSON'),
    help='If set, output the result as JSON',
)


def parse_version_string(version_str):
    if version_str.startswith(VERSIONPREFIX):  # remove version prefix
        version_str = version_str[len(VERSIONPREFIX):]

    parts = version_str.split('-')
    version_base = parts[0]
    version_rc = ""
    version_os = ""
    if len(parts) == 2:  # "<version>-rcN" or "version-platform"
        if "rc" in parts[1]:
            version_rc = parts[1]
        else:
            version_os = parts[1]
    elif len(parts) == 3:  # "<version>-rcN-platform"
        version_rc = parts[1]
        version_os = parts[2]

    return version_base, version_rc, version_os


def download_with_wget(remote_file, local_file):
    result = subprocess.run(['wget', '-O', local_file, remote_file],
                            stderr=subprocess.STDOUT, stdout=subprocess.PIPE)
    return result.returncode == 0, result.stdout.decode().rstrip()


def download_lines_with_urllib(url) -> t.Tuple[bool, t.List[str]]:
    """Get (success, text lines of a file) over HTTP."""
    try:
        return (True, [
            line.strip().decode() for line in urllib.request.urlopen(url).readlines()])
    except urllib.request.HTTPError as e:
        log.warning(f"HTTP request to {url} failed (HTTPError): {e}")
    except Exception as e:
        log.warning(f"HTTP request to {url} failed ({e})")
    return (False, [])


def verify_with_gpg(
    signature_filename,
    output_filename: t.Optional[str] = None
) -> t.Tuple[int, str]:
    args = [
        'gpg', '--yes', '--decrypt', '--verify-options', 'show-primary-uid-only',
        '--output', output_filename if output_filename else '', signature_filename]

    env = dict(os.environ, LANGUAGE='en')
    result = subprocess.run(args, stderr=subprocess.STDOUT, stdout=subprocess.PIPE, env=env)
    log.debug(f'Result from GPG ({result.returncode}): {result.stdout}')
    return result.returncode, result.stdout.decode().rstrip()


def remove_files(filenames):
    for filename in filenames:
        os.remove(filename)


class SigData:
    """GPG signature data as parsed from GPG stdout."""
    def __init__(self, key: str, name: str, trusted: bool, status: str):
        self.key = key
        self.name = name
        self.trusted = trusted
        self.status = status

    def __repr__(self):
        return (
            "SigData(%r, %r, trusted=%s, status=%r)" %
            (self.key, self.name, self.trusted, self.status))


def parse_gpg_result(
    output: t.List[str]
) -> t.Tuple[t.List[SigData], t.List[SigData], t.List[SigData]]:
    """Returns good, unknown, and bad signatures from GPG stdout."""
    good_sigs = []
    unknown_sigs = []
    bad_sigs = []
    total_resolved_sigs = 0
    curr_key = None

    # Ensure that all lines we match on include a prefix that prevents malicious input
    # from fooling the parser.
    def line_begins_with(patt: str, line: str) -> t.Optional[re.Match]:
        return re.match(r'^\s*(gpg:)?(\s+)' + patt, line)

    detected_name = ''

    for i, line in enumerate(output):
        if line_begins_with(r"using (ECDSA|RSA) key (0x[0-9a-fA-F]{16}|[0-9a-fA-F]{40})$", line):
            if curr_key:
                raise RuntimeError(
                    f"WARNING: encountered a new sig without resolving the last ({curr_key}) - "
                    "this could mean we have encountered a bad signature! check GPG output!")
            curr_key = line.split('key ')[-1].strip()
            assert len(curr_key) == 40 or (len(curr_key) == 18 and curr_key.startswith('0x'))

        if line_begins_with(r"Can't check signature: No public key$", line):
            if not curr_key:
                raise RuntimeError("failed to detect signature being resolved")
            unknown_sigs.append(SigData(curr_key, detected_name, False, ''))
            detected_name = ''
            curr_key = None

        if line_begins_with(r'Good signature from (".+")(\s+)(\[.+\])$', line):
            if not curr_key:
                raise RuntimeError("failed to detect signature being resolved")
            name, status = parse_gpg_from_line(line)

            # It's safe to index output[i + 1] because if we saw a good sig, there should
            # always be another line
            trusted = (
                'This key is not certified with a trusted signature' not in output[i + 1])
            good_sigs.append(SigData(curr_key, name, trusted, status))
            curr_key = None

        if line_begins_with("issuer ", line):
            detected_name = line.split("issuer ")[-1].strip('"')

        if 'bad signature from' in line.lower():
            if not curr_key:
                raise RuntimeError("failed to detect signature being resolved")
            name, status = parse_gpg_from_line(line)
            bad_sigs.append(SigData(curr_key, name, False, status))
            curr_key = None

        # Track total signatures included
        if line_begins_with('Signature made ', line):
            total_resolved_sigs += 1

    all_found = len(good_sigs + bad_sigs + unknown_sigs)
    if all_found != total_resolved_sigs:
        raise RuntimeError(
            f"failed to evaluate all signatures: found {all_found} "
            f"but expected {total_resolved_sigs}")

    return (good_sigs, unknown_sigs, bad_sigs)


def parse_gpg_from_line(line: str) -> t.Tuple[str, str]:
    """Returns name and expiration status."""
    assert 'signature from' in line

    name_end = line.split(' from ')[-1]
    m = re.search(r'(?P<name>".+") \[(?P<status>\w+)\]', name_end)
    assert m
    (name, status) = m.groups()
    name = name.strip('"\'')

    return (name, status)


def files_are_equal(filename1, filename2):
    with open(filename1, 'rb') as file1:
        contents1 = file1.read()
    with open(filename2, 'rb') as file2:
        contents2 = file2.read()
    eq = contents1 == contents2

    if not eq:
        with open(filename1, 'r', encoding='utf-8') as f1, \
                open(filename2, 'r', encoding='utf-8') as f2:
            f1lines = f1.readlines()
            f2lines = f2.readlines()

            diff = indent(
                ''.join(difflib.unified_diff(f1lines, f2lines)))
            log.warning(f"found diff in files ({filename1}, {filename2}):\n{diff}\n")

    return eq


def get_files_from_hosts_and_compare(
    hosts: t.List[str], path: str, filename: str, require_all: bool = False
) -> ReturnCode:
    """
    Retrieve the same file from a number of hosts and ensure they have the same contents.
    The first host given will be treated as the "primary" host, and is required to succeed.

    Args:
        filename: for writing the file locally.
    """
    assert len(hosts) > 1
    primary_host = hosts[0]
    other_hosts = hosts[1:]
    got_files = []

    def join_url(host: str) -> str:
        return host.rstrip('/') + '/' + path.lstrip('/')

    url = join_url(primary_host)
    success, output = download_with_wget(url, filename)
    if not success:
        log.error(
            f"couldn't fetch file ({url}). "
            "Have you specified the version number in the following format?\n"
            f"[{VERSIONPREFIX}]{VERSION_FORMAT} "
            f"(example: {VERSIONPREFIX}{VERSION_EXAMPLE})\n"
            f"wget output:\n{indent(output)}")
        return ReturnCode.FILE_GET_FAILED
    else:
        log.info(f"got file {url} as {filename}")
        got_files.append(filename)

    for i, host in enumerate(other_hosts):
        url = join_url(host)
        fname = filename + f'.{i + 2}'
        success, output = download_with_wget(url, fname)

        if require_all and not success:
            log.error(
                f"{host} failed to provide file ({url}), but {primary_host} did?\n"
                f"wget output:\n{indent(output)}")
            return ReturnCode.FILE_MISSING_FROM_ONE_HOST
        elif not success:
            log.warning(
                f"{host} failed to provide file ({url}). "
                f"Continuing based solely upon {primary_host}.")
        else:
            log.info(f"got file {url} as {fname}")
            got_files.append(fname)

    for i, got_file in enumerate(got_files):
        if got_file == got_files[-1]:
            break  # break on last file, nothing after it to compare to

        compare_to = got_files[i + 1]
        if not files_are_equal(got_file, compare_to):
            log.error(f"files not equal: {got_file} and {compare_to}")
            return ReturnCode.FILES_NOT_EQUAL

    return ReturnCode.SUCCESS


def check_multisig(sigfilename: str, args: argparse.Namespace):
    # check signature
    #
    # We don't write output to a file because this command will almost certainly
    # fail with GPG exit code '2' (and so not writing to --output) because of the
    # likely presence of multiple untrusted signatures.
    retval, output = verify_with_gpg(sigfilename)

    if args.verbose:
        log.info(f"gpg output:\n{indent(output)}")

    good, unknown, bad = parse_gpg_result(output.splitlines())

    if unknown and args.import_keys:
        # Retrieve unknown keys and then try GPG again.
        for unsig in unknown:
            if prompt_yn(f" ? Retrieve key {unsig.key} ({unsig.name})? (y/N) "):
                ran = subprocess.run(
                    ["gpg", "--keyserver", args.keyserver, "--recv-keys", unsig.key])

                if ran.returncode != 0:
                    log.warning(f"failed to retrieve key {unsig.key}")

        # Reparse the GPG output now that we have more keys
        retval, output = verify_with_gpg(sigfilename)
        good, unknown, bad = parse_gpg_result(output.splitlines())

    return retval, output, good, unknown, bad


def prompt_yn(prompt) -> bool:
    """Return true if the user inputs 'y'."""
    got = ''
    while got not in ['y', 'n']:
        got = input(prompt).lower()
    return got == 'y'


def main(args):
    args = parser.parse_args()
    if args.quiet:
        log.setLevel(logging.WARNING)

    WORKINGDIR = Path(tempfile.gettempdir()) / f"bitcoin_verify_binaries.{args.version}"

    def cleanup():
        log.info("cleaning up files")
        os.chdir(Path.home())
        shutil.rmtree(WORKINGDIR)

    # determine remote dir dependent on provided version string
    try:
        version_base, version_rc, os_filter = parse_version_string(args.version)
        version_tuple = [int(i) for i in version_base.split('.')]
    except Exception as e:
        log.debug(e)
        log.error(f"unable to parse version; expected format is {VERSION_FORMAT}")
        log.error(f"  e.g. {VERSION_EXAMPLE}")
        return ReturnCode.BAD_VERSION

    remote_dir = f"/bin/{VERSIONPREFIX}{version_base}/"
    if version_rc:
        remote_dir += f"test.{version_rc}/"
    remote_sigs_path = remote_dir + SIGNATUREFILENAME
    remote_sums_path = remote_dir + SUMS_FILENAME

    # create working directory
    os.makedirs(WORKINGDIR, exist_ok=True)
    os.chdir(WORKINGDIR)

    hosts = [HOST1, HOST2]

    got_sig_status = get_files_from_hosts_and_compare(
        hosts, remote_sigs_path, SIGNATUREFILENAME, args.require_all_hosts)
    if got_sig_status != ReturnCode.SUCCESS:
        return got_sig_status

    # Multi-sig verification is available after 22.0.
    if version_tuple[0] >= 22:
        min_good_sigs = args.min_good_sigs
        gpg_allowed_codes = [0, 2]  # 2 is returned when untrusted signatures are present.

        got_sums_status = get_files_from_hosts_and_compare(
            hosts, remote_sums_path, SUMS_FILENAME, args.require_all_hosts)
        if got_sums_status != ReturnCode.SUCCESS:
            return got_sums_status

        gpg_retval, gpg_output, good, unknown, bad = check_multisig(SIGNATUREFILENAME, args)
    else:
        log.error("Version too old - single sig not supported. Use a previous "
                  "version of this script from the repo.")
        return ReturnCode.BAD_VERSION

    if gpg_retval not in gpg_allowed_codes:
        if gpg_retval == 1:
            log.critical(f"Bad signature (code: {gpg_retval}).")
        if gpg_retval == 2:
            log.critical(
                "gpg error. Do you have the Bitcoin Core binary release "
                "signing key installed?")
        else:
            log.critical(f"unexpected GPG exit code ({gpg_retval})")

        log.error(f"gpg output:\n{indent(gpg_output)}")
        cleanup()
        return ReturnCode.INTEGRITY_FAILURE

    # Decide which keys we trust, though not "trust" in the GPG sense, but rather
    # which pubkeys convince us that this sums file is legitimate. In other words,
    # which pubkeys within the Bitcoin community do we trust for the purposes of
    # binary verification?
    trusted_keys = set()
    if args.trusted_keys:
        trusted_keys |= set(args.trusted_keys.split(','))

    # Tally signatures and make sure we have enough goods to fulfill
    # our threshold.
    good_trusted = {sig for sig in good if sig.trusted or sig.key in trusted_keys}
    good_untrusted = [sig for sig in good if sig not in good_trusted]
    num_trusted = len(good_trusted) + len(good_untrusted)
    log.info(f"got {num_trusted} good signatures")

    if num_trusted < min_good_sigs:
        log.info("Maybe you need to import "
                  f"(`gpg --keyserver {args.keyserver} --recv-keys <key-id>`) "
                  "some of the following keys: ")
        log.info('')
        for sig in unknown:
            log.info(f"    {sig.key} ({sig.name})")
        log.info('')
        log.error(
            "not enough trusted sigs to meet threshold "
            f"({num_trusted} vs. {min_good_sigs})")

        return ReturnCode.NOT_ENOUGH_GOOD_SIGS

    for sig in good_trusted:
        log.info(f"GOOD SIGNATURE: {sig}")

    for sig in good_untrusted:
        log.info(f"GOOD SIGNATURE (untrusted): {sig}")

    for sig in [sig for sig in good if sig.status == 'expired']:
        log.warning(f"key {sig.key} for {sig.name} is expired")

    for sig in bad:
        log.warning(f"BAD SIGNATURE: {sig}")

    for sig in unknown:
        log.warning(f"UNKNOWN SIGNATURE: {sig}")

    # extract hashes/filenames of binaries to verify from hash file;
    # each line has the following format: "<hash> <binary_filename>"
    with open(SUMS_FILENAME, 'r', encoding='utf8') as hash_file:
        hashes_to_verify = [line.split()[:2] for line in hash_file if os_filter in line]
    remove_files([SUMS_FILENAME])
    if not hashes_to_verify:
        log.error("no files matched the platform specified")
        return ReturnCode.NO_BINARIES_MATCH

    # remove binaries that are known not to be hosted by bitcoincore.org
    fragments_to_remove = ['-unsigned', '-debug', '-codesignatures']
    for fragment in fragments_to_remove:
        nobinaries = [i for i in hashes_to_verify if fragment in i[1]]
        if nobinaries:
            remove_str = ', '.join(i[1] for i in nobinaries)
            log.info(
                f"removing *{fragment} binaries ({remove_str}) from verification "
                f"since {HOST1} does not host *{fragment} binaries")
            hashes_to_verify = [i for i in hashes_to_verify if fragment not in i[1]]

    # download binaries
    for _, binary_filename in hashes_to_verify:
        log.info(f"downloading {binary_filename}")
        success, output = download_with_wget(
            HOST1 + remote_dir + binary_filename, binary_filename)

        if not success:
            log.error(
                f"failed to download {binary_filename}\n"
                f"wget output:\n{indent(output)}")
            return ReturnCode.BINARY_DOWNLOAD_FAILED

    # verify hashes
    offending_files = []
    files_to_hashes = {}

    for hash_expected, binary_filename in hashes_to_verify:
        with open(binary_filename, 'rb') as binary_file:
            hash_calculated = sha256(binary_file.read()).hexdigest()
        if hash_calculated != hash_expected:
            offending_files.append(binary_filename)
        else:
            files_to_hashes[binary_filename] = hash_calculated

    if offending_files:
        joined_files = '\n'.join(offending_files)
        log.critical(
            "Hashes don't match.\n"
            f"Offending files:\n{joined_files}")
        return ReturnCode.INTEGRITY_FAILURE

    if args.cleanup:
        cleanup()
    else:
        log.info(f"did not clean up {WORKINGDIR}")

    if args.json:
        output = {
            'good_trusted_sigs': [str(s) for s in good_trusted],
            'good_untrusted_sigs': [str(s) for s in good_untrusted],
            'unknown_sigs': [str(s) for s in unknown],
            'bad_sigs': [str(s) for s in bad],
            'verified_binaries': files_to_hashes,
        }
        print(json.dumps(output, indent=2))
    else:
        for filename in files_to_hashes:
            print(f"VERIFIED: {filename}")

    return ReturnCode.SUCCESS


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
