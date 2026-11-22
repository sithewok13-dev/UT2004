"""Signs the app with a development or ad hoc identity, so it installs on the devices of its provisioning profile.

The entitlements come from the profile, its application identifier must match UT_IOS_BUNDLE_ID.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', required=True)
    p.add_argument('--identity', required=True, help='as codesign -s takes it, e.g. "Apple Development: Name (TEAMID)"')
    p.add_argument('--profile', required=True, help='a .mobileprovision file')
    a = p.parse_args()

    profile = plistlib.loads(subprocess.check_output(['security', 'cms', '-D', '-i', a.profile]))
    shutil.copyfile(a.profile, os.path.join(a.app, 'embedded.mobileprovision'))
    with tempfile.NamedTemporaryFile('wb', suffix='.plist', delete=False) as f:
        plistlib.dump(profile['Entitlements'], f)
    try:
        subprocess.check_call(['codesign', '--force', '--sign', a.identity, '--entitlements', f.name,
                               '--timestamp=none', '--generate-entitlement-der', a.app])
    finally:
        os.remove(f.name)


if __name__ == '__main__':
    main()
