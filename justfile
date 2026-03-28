build:
    ninja -C build lld

pack:
    npx repomix lld/COFF/

zip:
    tar -cf lld-coff.zip --format=zip lld/COFF/
