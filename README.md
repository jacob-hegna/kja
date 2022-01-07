```sh
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DKJA_BUILD_TESTS=1 \
  -DKJA_BUILD_BENCH=1 \
  -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
  ..
```
