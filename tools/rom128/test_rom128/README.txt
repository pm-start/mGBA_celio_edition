128MB ROM の窓の単体試験（C）。mGBA のビルド後に MSYS2 mingw64 で（定義はビルドの DEFINES と同じにする）:
  gcc -O1 -DBUILD_STATIC -DENABLE_DEBUGGERS -DENABLE_DIRECTORIES -DENABLE_GDB_STUB -DENABLE_SCRIPTING -DENABLE_VFS -DENABLE_VFS_FD -DHAVE_CRC32 \
      -DM_CORE_GB -DM_CORE_GBA -DUSE_LUA -DUSE_PNG -DUSE_ZLIB -DUSE_LIBZIP -DUSE_LZMA -DUSE_SQLITE3 -DUSE_JSON_C -D_GNU_SOURCE \
      -I include -I <build>/include tools/rom128/test_rom128/rom128.c <build>/libmgba.a \
      -lz -lm -lzip -lshlwapi -lole32 -lshell32 -luuid -lws2_32 -lpng -lfreetype -llua -lpthread -lepoxy -ljson-c -lSDL2 -lsqlite3 -lbz2 -lharfbuzz -lstdc++ -o rom128.exe
  rom128.exe <作業フォルダー>      （終了コード 0 ＝ 全部通った・50 項目）
見る事: 128MB の窓の既定(バンク1)・切り替え(2・3)・8/16/32 ビット読み・命令の実行・合言葉の誤り・別番地/別幅の書き込みの拒否・ステート往復・リセット・64MB／32MB が今と同じ。
  （FFMPEG=ON で組んだときは、リンクの最後に -lavformat -lavcodec -lavutil -lswscale -lswresample を足す）
