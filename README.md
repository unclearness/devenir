# devenir: Interactive Rigid/Non-Rigid ICP Tool for Meshes

## Features

### Manual point annotation

- Put and move points on triangular faces (face id + barycentric)
- Import/export Wrap3 compatible json formats

![devenir_points_20230506](https://user-images.githubusercontent.com/1129855/236621264-07d55793-fced-4349-8114-a50dc9d283aa.gif)

### Registration

- Correspondence based registration (rigid + scale)
- Rigid ICP
- Non-Rigid ICP
- Texture transfer

![devenir_registration_2_20230506](https://user-images.githubusercontent.com/1129855/236632057-db247b4f-950a-483f-b2da-d37a2f749934.gif)


## Build

Linux (clang/gcc) and Windows (MSVC) are supported.

- `git submodule update --init --recursive`
  - To pull dependencies registered as git submodule.
- Use CMake with `CMakeLists.txt`.
  - `reconfigure.bat` and `rebuild.bat` are command line CMake utilities for Windows 10/11 and Visual Studio 2017-2022.
