set -ex && mkdir -p build/release/bin
set -ex && docker create --name sispop-daemon-container sispop-daemon-image
set -ex && docker cp sispop-daemon-container:/usr/local/bin/ build/release/
set -ex && docker rm sispop-daemon-container
