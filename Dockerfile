FROM amazonlinux as build

LABEL name="blaze as a lambda function"
LABEL version="1.0.0"
LABEL maintainer="norman@khine.net"

RUN yum install -y git

RUN yum install -y gcc gcc-c++ \
                   libtool libtool-ltdl \
                   make cmake \
                   pkgconfig automake autoconf && \
    yum clean all

RUN set -eux && \
  cd /tmp && \
  git clone https://github.com/curl/curl.git && \
  cd curl && \
  ./buildconf && \
  ./configure && \
  make  && \
  make install
RUN set -eux && \
  cd /tmp && \
  git clone https://github.com/unidentifieddeveloper/blaze.git && \
  cd blaze && \
  git submodule update --init && \
  make

FROM amazonlinux
WORKDIR /
COPY --from=build /tmp/blaze/blaze /usr/local/bin/
CMD ["/usr/local/bin/blaze"]