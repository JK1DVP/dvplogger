# ライセンスヘッダ定義
license_text='/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2025 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */'

# 対象ファイルを指定
find main -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.py" \) | while read file; do
    echo "Processing $file"
    tmpfile="$file.tmp"

    # 既存のSPDXヘッダを削除（コメントブロック内）
    awk '
    BEGIN {skip=0}
    # コメントブロック開始
    /^\s*\/\*/ {skip=1}
    # SPDX行発見でスキップモード
    skip==1 && /SPDX-License-Identifier:/ {next}
    # コメントブロック終了
    skip==1 && /\*\// {skip=0; next}
    # スキップ中は何も出力しない
    skip==1 {next}
    {print}
    ' "$file" > "$tmpfile"

    # 新しいヘッダを先頭に追加
    echo "$license_text" | cat - "$tmpfile" > "$file"
    rm "$tmpfile"
done
exit
# ライセンスヘッダ定義
license_text='/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2025 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */'

# 対象ファイルを指定（C, H, CPP, PYなど）
find main -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.py" \) | while read file; do
    # 既に SPDX があるか確認
    if ! grep -q "SPDX-License-Identifier" "$file"; then
        echo "Adding license header to $file"
        tmpfile="$file.tmp"
        # 既存コメントの直後にヘッダ追加
        awk -v header="$license_text" '
        NR==1{print; next}
        NR==2{sub(/^/, header "\n\n")}
        1' "$file" > "$tmpfile" && mv "$tmpfile" "$file"
    else
        echo "Skipping $file (already has SPDX)"
    fi
done
