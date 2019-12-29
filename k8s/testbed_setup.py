import subprocess
import os
import time
import re

import signal
import sys


def signal_handler(sig, frame):
    print('You pressed Ctrl+C!')
    os.system("sudo k3d delete")
    sys.exit(0)


signal.signal(signal.SIGINT, signal_handler)

my_env = os.environ.copy()


os.system("sudo k3d delete")
os.system("sudo k3d create --publish 7000:32700@k3d-k3s-default-worker-0 --workers 1")

while True:
    try:
        print("Trying to get env variable name...")
        command = "k3d get-kubeconfig -name k3s-default"
        env_var = subprocess.check_output(["sudo", "-S"] + command.split(" "), env=my_env).strip().decode("utf-8")
        break
    except subprocess.CalledProcessError as e:
        print(e)
        pass
    time.sleep(2)
print(env_var)

my_env["KUBECONFIG"] = env_var

subprocess.check_output("kubectl apply -f redis.yaml".split(" "), env=my_env)

while True:
    try:
        output = subprocess.check_output("kubectl get deployment".split(" "), env=my_env).decode("utf-8")
        print(output)
        finds = re.findall(r"[0-9]+/[0-9]+", output)
        all_good = True
        for find in finds:
            pieces = find.strip().split("/")
            all_good = all_good and int(pieces[0]) == int(pieces[1])
        if all_good:
            break
    except subprocess.CalledProcessError:
        pass
    time.sleep(5)

subprocess.check_output("kubectl apply -f worker.yaml".split(" "), env=my_env)

while True:
    try:
        output = subprocess.check_output("kubectl get deployment".split(" "), env=my_env).decode("utf-8")
        print(output)
        finds = re.findall(r"[0-9]+/[0-9]+", output)
        all_good = True
        for find in finds:
            pieces = find.strip().split("/")
            all_good = all_good and int(pieces[0]) == int(pieces[1])
        if all_good:
            break
    except subprocess.CalledProcessError:
        pass
    time.sleep(5)

os.system("export KUBECONFIG={} && kubectl apply -f load_gen.yaml && python ../ctls/bangbang.py".format(env_var))
