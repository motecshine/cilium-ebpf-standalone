# Standalone 

## Init development env for vscode

```Shell
apt install clang-17 clangd-17
update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-17 100
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 100
pip3 install compiledb
compiledb --parse make_output.txt
make clean;make VERBOSE=y all &> make_output.txt
make -j$(nproc)
```