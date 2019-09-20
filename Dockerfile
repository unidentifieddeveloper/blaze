# -- build context

FROM alpine AS build-env

LABEL name="blaze in docker"
LABEL version="1.0.0"
LABEL maintainer="norman@khine.net"

WORKDIR /tmp

RUN apk update \
  && apk add --no-cache g++ gcc automake make autoconf libtool openssl-dev curl-dev git \
  && git clone --recurse-submodules https://github.com/unidentifieddeveloper/blaze.git . \
  && make

# -- runtime context

FROM alpine

RUN apk update \
  && apk add --no-cache ca-certificates openssl \
  && update-ca-certificates

COPY --from=build-env /lib /lib
COPY --from=build-env /usr/lib /usr/lib
COPY --from=build-env /usr/local/share /usr/local/share
COPY --from=build-env /usr/local/lib /usr/local/lib
COPY --from=build-env /tmp/blaze /usr/local/bin

CMD ["/usr/local/bin/blaze"]
