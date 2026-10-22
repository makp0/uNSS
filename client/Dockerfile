FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

# The official image includes Switch portlibs. Check them offline: fetching
# package databases in CI is unsupported and can fail with HTTP 403.
RUN dkp-pacman -Q \
    switch-curl \
    switch-sdl2 \
    switch-sdl2_ttf \
    switch-freetype \
    switch-harfbuzz \
    switch-mesa \
    switch-libpng

ENV DEVKITPRO=/opt/devkitpro
ENV PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:/opt/devkitpro/devkitA64/bin:${PATH}

WORKDIR /client
