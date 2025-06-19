#!/usr/bin/env python3
"""
Script to switch between Chinese and English Kconfig.projbuild versions
Usage: python scripts/switch_kconfig_language.py [zh|en]
"""

import sys
from pathlib import Path

def switch_kconfig_language(language):
    """Switch Kconfig.projbuild to the specified language version (zh or en)"""
    project_root = Path(__file__).parent.parent
    main_dir = project_root / "main"
    
    kconfig_target = main_dir / "Kconfig.projbuild"
    kconfig_zh = main_dir / "Kconfig.projbuild.zh"
    kconfig_en = main_dir / "Kconfig.projbuild.en"

    if language.lower() == "zh":
        if not kconfig_zh.exists():
            print("❌ Chinese version (Kconfig.projbuild.zh) not found.")
            return False
        kconfig_target.write_text(kconfig_zh.read_text(encoding="utf-8"), encoding="utf-8")
        print("✅ Switched to Chinese Kconfig.projbuild.")
        return True
    elif language.lower() == "en":
        if not kconfig_en.exists():
            print("❌ English version (Kconfig.projbuild.en) not found.")
            return False
        kconfig_target.write_text(kconfig_en.read_text(encoding="utf-8"), encoding="utf-8")
        print("✅ Switched to English Kconfig.projbuild.")
        return True
    else:
        print("❌ Invalid language. Use 'zh' for Chinese or 'en' for English.")
        return False

def main():
    if len(sys.argv) != 2:
        print("Usage: python scripts/switch_kconfig_language.py [zh|en]")
        return
    language = sys.argv[1]
    switch_kconfig_language(language)

if __name__ == "__main__":
    main()