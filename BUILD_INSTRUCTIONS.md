# Steps to build for vscode debugger

First need to install emscripten and clone emsdk:

```shell
brew install emscripten
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
```

Init emsdk env and build with cmake:

```shell
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build
cd build
cmake --build . -j8
```

Copy files to plugin dir:

```shell
cp vAmiga.js ../../vamiga/
cp vAmiga.wasm ../../vamiga/
```
