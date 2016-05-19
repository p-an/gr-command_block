# gr-command_block
Set of gnuradio blocks, which can use any command as source, sink or filter. Inspired by https://github.com/jolivain/gr-pipe

###### Build
```
git clone https://github.com/p-an/gr-command_block
cd gr-command_block
mkdir build
cd build
cmake ../
make 
```

###### install
```
sudo make install
```

###### use
Run gnuradio like this:
```
LD_LIBRARY_PATH=/usr/local/lib PYTHONPATH=/usr/local/lib/python2.7/dist-packages/command_block/ gnuradio-companion
```
