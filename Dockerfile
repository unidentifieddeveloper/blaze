
FROM alpine AS build

LABEL name="blaze in docker"
LABEL version="1.0.0"
LABEL maintainer="norman@khine.net"

RUN apk update \
  && apk add --no-cache g++ gcc automake make autoconf libtool libressl-dev git \
  && cd /tmp \
  && git clone https://github.com/curl/curl.git \
  && cd curl \
  && ./buildconf \
  && ./configure --with-ssl \
  && make \
  && make install \
  && cd /tmp \
  && git clone https://github.com/unidentifieddeveloper/blaze.git \
  && cd blaze \
  && git submodule update --init \
  && make

FROM alpine
WORKDIR /
COPY --from=build /lib /lib
COPY --from=build /usr/lib /usr/lib
COPY --from=build /usr/local/share /usr/local/share
COPY --from=build /usr/local/include /usr/local/include
COPY --from=build /usr/local/lib /usr/local/lib
COPY --from=build /tmp/blaze/blaze /usr/local/bin
CMD ["/usr/local/bin/blaze"]
