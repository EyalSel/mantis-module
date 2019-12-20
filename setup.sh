set -ex

docker run -d\
	--name mantis-dev \
	--network=host \
	-v /var/run/docker.sock:/var/run/docker.sock \
	ubuntu:18.04 bash -c "/bin/bash"

