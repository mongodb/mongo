import re
import subprocess
import sys


def findMongoCertsFromStore(store):
    command = ["certutil", "-store", store]
    subject_pattern = re.compile(r"Subject:.*O=MongoDB")
    cn_pattern = re.compile(r"CN=([^,]+)")
    cns = []

    try:
        output = subprocess.check_output(command, shell=True).decode("utf-8")
    except subprocess.CalledProcessError as e:
        print(f"Command {command} failed with error: {e}", file=sys.stderr)
        sys.exit(1)

    filtered = [s for s in output.splitlines() if re.match(subject_pattern, s)]
    for line in filtered:
        cn_match = re.search(cn_pattern, line)
        if cn_match:
            cns.append(cn_match.group(1))
    return cns


def deleteCertsByCNFromStore(store, cns):
    command = ["certutil", "-delstore", "-f", store, "cn"]
    for cn in cns:
        command[4] = cn
        try:
            print(
                f"Deleting 'CN={cn}' from the '{store}' certificate store:\n\t{' ' .join(command)}"
            )
            subprocess.check_call(command, shell=True)
        except subprocess.CalledProcessError as e:
            print(f"Command {command} failed with error: {e}", file=sys.stderr)
            sys.exit(1)


my_cns = findMongoCertsFromStore("My")
root_cns = findMongoCertsFromStore("Root")

if my_cns + root_cns:
    print("Unexpected MongoDB certs found on host. Clearing them from the system cert stores.")
deleteCertsByCNFromStore("My", my_cns)
deleteCertsByCNFromStore("Root", root_cns)
