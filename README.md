# Standalone 

## build

```json
docker run --rm \
	-v "$PWD":/go/src/github.com/cilium/cilium \
	-w /go/src/github.com/cilium/cilium \
	quay.io/cilium/cilium-builder@sha256:ce77dd34bfbd8cefe220858d0ba8ab369f07cd5afc3fd189f2c4eb78ef643669 \
	"/bin/bash make -j$(nproc)"
```