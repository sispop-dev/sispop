RELEASE=9.1.0
docker build -t sispopd:${RELEASE} -f Dockerfile.sispopd --build-arg USER_ID=$(id -u) --build-arg GROUP_ID=$(id -g) .
