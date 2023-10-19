RELEASE=20.04
curl -so sispop-deb-key.gpg https://deb.sispop.io/pub.gpg
docker build -t sispop-ubuntu:${RELEASE} -f Dockerfile.sispop-ubuntu .
rm sispop-deb-key.gpg
