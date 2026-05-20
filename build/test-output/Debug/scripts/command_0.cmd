@echo off
"E:/GitHub/single_header/build/Debug/single_header.exe" --config "E:/GitHub/single_header/build/test-output/Debug\config_override\custom.json" --output "E:/GitHub/single_header/build/test-output/Debug\config_override\generated\from_cli.hpp" --rewrite-macro CSV_INLINE=inline --implementation-marker "/** INSERT_CSV_SOURCES **/"
