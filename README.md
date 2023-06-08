# Caissa Chess Engine

[![LinuxBuildStatus](https://github.com/Witek902/Caissa/workflows/Linux/badge.svg)](https://github.com/Witek902/Caissa/actions/workflows/linux.yml)

![ArtImage](https://user-images.githubusercontent.com/5882734/193368109-abce432b-85e9-4f11-bb3c-57fd3d27db22.jpg?raw=true)
<p style='text-align: right;'><em>(image generated with DALL·E 2)</em></p>

## Overview

Strong, UCI command-line chess engine, written from scratch in C++ in development since early 2021.

## Playing strength

Caissa is listed on many chess engines ranking lists:

* [CCRL 40/2 FRC](https://ccrl.chessdom.com/ccrl/404FRC/) - **3743** (#6) (version 1.8)
* [CCRL 40/15](https://ccrl.chessdom.com/ccrl/4040/) - **3434** (#15) (version 1.8)
* [CCRL 40/2](https://ccrl.chessdom.com/ccrl/404/) - **3588** (#18) (version 1.7)
* [SPCC](https://www.sp-cc.de) - **3546** (#21) (version 1.8)
* [IpMan Chess 5+0](https://ipmanchess.yolasite.com/i7-11800h.php) - **3381** (#27) (version 1.8)
* [IpMan Chess 10+1](https://ipmanchess.yolasite.com/i9-7980xe.php) - **3311** (#27) (version 1.8.4 avx512)
* [CEGT 40/20](http://www.cegt.net/40_40%20Rating%20List/40_40%20SingleVersion/rangliste.html) - **3401** (#22) (version 1.8)
* [CEGT 40/4](http://www.cegt.net/40_4_Ratinglist/40_4_single/rangliste.html) - **3399** (#20) (version 1.7)
* [CEGT 5+3](http://www.cegt.net/5Plus3Rating/BestVersionsNEW/rangliste.html) - **3382** (#21) (version 1.6.3)
* [FGRL](http://www.fastgm.de/60-0.60.html) - **3253** (#18) (version 1.5)

## History / Originality

The engine has been written from the ground up. In early versions it used a simple PeSTO evaluation, which was replaced by the Stockfish NNUE for a short time. Since version 0.7, Caissa uses it's own efficiently updated neural network, trained with Caissa self-play games using a custom trainer. In a way, the first own Caissa network is based on Stockfish's network, but it was much weaker because of the small data set used back then (a few million positions). Currently (as of version 1.9) over 1.6 billion newly generated positions are used. Also, the old self-play games are successively purged, so that the newer networks are trained only on the most recent games generated by the most recent network, and so on.

The runtime neural network evaluation code is located in [PackedNeuralNetwork.cpp](https://github.com/Witek902/Caissa/blob/devel/src/backend/PackedNeuralNetwork.cpp) and was inspired by [nnue.md document](https://github.com/glinscott/nnue-pytorch/blob/master/docs/nnue.md). The neural network trainer is written completely from scratch and is located in [NetworkTrainer.cpp](https://github.com/Witek902/Caissa/blob/devel/src/utils/NetworkTrainer.cpp), [NeuralNetwork.cpp](https://github.com/Witek902/Caissa/blob/devel/src/utils/NeuralNetwork.cpp) and other NeuralNetwork* files. The trainer is purely CPU-based and is heavily optimized to take advantage of many threads and AVX instructions as well as it exploits the sparse nature of the nets.

The games are generated with the utility [SelfPlay.cpp](https://github.com/Witek902/Caissa/blob/devel/src/utils/SelfPlay.cpp), which generates games with a fixed number of nodes/depth and saves them in a custom binary game format to save space. The opening books used are either Stefan's Pohl [UHO books](https://www.sp-cc.de/uho_2022.htm) or DFRC (Double Fischer Random Chess) openings with few random moves played at the beginning.

### Supported UCI options

* **Hash** (int) Sets the size of the transposition table in megabytes.
* **MultiPV** (int) Sets the number of PV lines to search and print.
* **MoveOverhead** (int) Sets move overhead in milliseconds. Should be increased if the engine loses time.
* **Threads** (int) Sets the number of threads used for searching.
* **Ponder** (bool) Enables pondering.
* **EvalFile** (string) Neural network evaluation file.
* **SyzygyPath** (string) Semicolon-separated list of paths to Syzygy endgame tablebases.
* **SyzygyProbeLimit** (int) Maximum number of pieces on the board where Syzygy tablebases can be used.
* **GaviotaTbPath** (string) Path to Gaviota endgame tablebases.
* **GaviotaTbCache** (int) Gaviota cache size in megabytes.
* **UCI_AnalyseMode** (bool) Enables analysis mode: search full PV lines and disable any depth constraints.
* **UseSAN** (bool) Enables short algebraic notation output (FIDE standard) instead of default long algebraic notation.
* **ColorConsoleOutput** (bool) Enables colored console output for better readability.


### Provided EXE versions

* **AVX-512** - Fastest, requires a x64 CPU with AVX-512 instruction set support. May not be supported.
* **AVX2/BMI2** - Fast, requires a x64 CPU with AVX2 and BMI2 instruction set support. Supported by majority of CPUs.
* **POPCNT** - Slower, requires a x64 CPU with SSE4 and POPCNT instruction set support. For older CPUs.
* **Legacy** - Slowest, requires any x64 CPU. For very old x64 CPUs.


## Features

#### General
* UCI protocol
* Neural network evaluation
* Syzygy and Gaviota endgame tablebases support
* Chess960 (Fischer Random) support

#### Search Algorithm
* Negamax with alpha-beta pruning
* Iterative Deepening with Aspiration Windows
* Principal Variation Search (PVS)
* Quiescence Search
* Transposition Table
* Multi-PV search
* Multithreaded search via shared transposition table

#### Evaluation
* Neural network evaluation
  * 704&rarr;1536&rarr;1 architecture
  * effectively updated first layer
  * manually vectorized code supporting SSE2, AVX2, AVX-512 and ARM NEON instructions
  * clipped-ReLU activation function
  * 16 variants (aka. buckets) of last layer weights selected based on piece count and queen presence
  * input features: absolute piece coordinates with horizontal symmetry, no king-relative features
* Special endgame evaluation routines

#### Neural net trainer
* Custom CPU-based trainer using Adadelta algorithm
* Heavily optimized using AVX instructions, multithreading, and exploiting sparsity of the first layer input
* Network trained on data generated from self-play games (mixture of regular chess, FRC and DFRC games, over 1 billion positions in total)

#### Selectivity
* Null Move Reductions
* Late Move Reductions & Pruning
* Futility Pruning
* Mate Distance Pruning
* Singular Move Extensions
* Upcoming repetition detection

#### Move Ordering
* Most Valuable Victim + capture history
* Winning/Losing Captures (Static Exchange Evaluation)
* Killer/History/Counter/Followup Move Heuristic
* Sacrifice penalty / threat bonus
* Custom ordering for nodes near the root based on time spent on move

#### Time Management
* Heuristics based on approximate move count left and score fluctuations.
* Reducing search time for singular root moves

#### Misc
* Large Pages Support for Transposition Table
* Magic Bitboards
* Handling non-standard chess positions (e.g. 64 pieces on the board, etc.)
* Outstanding performance at ultra-short games (sub-second for whole game).

## Modules

The projects comprises following modules:
  * _backend_ (library) - engine's core
  * _frontend_ (executable) - UCI wrapper for the backend
  * _utils_ (executable) - various utilities, such as unit tests, neural network trainer, self-play data generator, etc.


## Compilation

### Linux

To compile for Linux use CMake:
```
mkdir build; cd build
cmake -DCMAKE_BUILD_TYPE=Final ..
make
```

**NOTE:** Currently, the project compiles with AVX2/BMI2 support by default. If your CPU does not support these instructions, you will need to manually modify the main CMakeLists.txt file.

There are three configurations supported:
* **Final** - final version, without asserts, etc.
* **Release** - development version with asserts enabled and with optimizations enabled for better performance
* **Debug** - development version with asserts enabled and optimizations disabled

### Windows

To compile for Windows, use `GenerateVisualStudioSolution.bat` to generate Visual Studio solution. The only tested Visual Studio version is 2022. Using CMake directly in Visual Studio was not tested.

After compilation make sure you copy appropriate neural net file from `data/neuralNets` directory to location where executable file is generated (`build/bin` on Linux or `build\bin\x64\<Configuration>` on Windows).
