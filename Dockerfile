FROM ubuntu:24.04 AS base
ENV DEBIAN_FRONTEND=noninteractive

FROM base AS build
RUN --mount=type=tmpfs,target=/tmp,rw \
    --mount=id=var_cache_apt,type=cache,target=/var/cache/apt,sharing=locked \
    --mount=id=var_lib_apt,type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update --error-on=any && \
    apt-get upgrade -y && \
    apt-get install -y build-essential libhwloc-dev libnuma-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/sc-membench
COPY . .

RUN make full && \
    cp membench-full /usr/local/bin/membench && \
    chmod +x /usr/local/bin/membench

FROM base AS final
RUN --mount=type=tmpfs,target=/tmp,rw \
    --mount=id=var_cache_apt_final,type=cache,target=/var/cache/apt,sharing=locked \
    --mount=id=var_lib_apt_final,type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update --error-on=any && \
    apt-get upgrade -y && \
    apt-get install -y libhwloc15 libnuma1 libhugetlbfs-dev libgomp1 && \
    rm -rf /var/lib/apt/lists/*

ENV PATH=${PATH}:/usr/local/bin
COPY --from=build /usr/local/bin/membench /usr/local/bin/membench

LABEL org.opencontainers.image.source="https://github.com/SpareCores/sc-membench"
LABEL org.opencontainers.image.url="https://github.com/SpareCores/sc-membench"

ENTRYPOINT ["nice", "-n", "-20", "/usr/local/bin/membench"]
