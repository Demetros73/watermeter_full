import subprocess
from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
platform = env.PioPlatform()

board = env.BoardConfig()
upload = board.get("upload", {})
upload["maximum_size"] = 3932160
board.update("upload", upload)

data_file = project_dir / "src" / "web" / "index.html"
source_file = build_dir / "index.html.S"
cmake = Path(platform.get_package_dir("tool-cmake")) / "bin" / "cmake.exe"
embed_script = (
    Path(platform.get_package_dir("framework-espidf"))
    / "tools"
    / "cmake"
    / "scripts"
    / "data_file_embed_asm.cmake"
)

build_dir.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [
        str(cmake),
        "-D",
        "DATA_FILE=" + str(data_file),
        "-D",
        "SOURCE_FILE=" + str(source_file),
        "-D",
        "FILE_TYPE=TEXT",
        "-P",
        str(embed_script),
    ],
    check=True,
)
