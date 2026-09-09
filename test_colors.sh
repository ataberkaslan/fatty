#!/usr/bin/env bash
# test_colors.sh - Visual test suite for f4tty ANSI colors and attributes

set -e

echo -e "\n=== f4tty ANSI Color & Style Test Suite ===\n"

# 1. Standard 8 Colors (Normal vs Bold)
echo "--- Standard 8 Foreground Colors (30-37) ---"
printf "%-12s %-20s %-20s\n" "Color" "Normal" "Bold"
colors=("Black" "Red" "Green" "Yellow" "Blue" "Magenta" "Cyan" "White")
for i in {0..7}; do
    fg=$((30 + i))
    name="${colors[$i]} ($fg)"
    normal="\e[${fg}mNormal Text\e[0m"
    bold="\e[${fg};1mBold Text\e[0m"
    printf "%-12s %-30b %-30b\n" "$name" "$normal" "$bold"
done

# 2. Bright 8 Colors
echo -e "\n--- Bright 8 Foreground Colors (90-97) ---"
printf "%-16s %-20s %-20s\n" "Color" "Bright" "Bright Bold"
bright_colors=("Br.Black" "Br.Red" "Br.Green" "Br.Yellow" "Br.Blue" "Br.Magenta" "Br.Cyan" "Br.White")
for i in {0..7}; do
    fg=$((90 + i))
    name="${bright_colors[$i]} ($fg)"
    bright="\e[${fg}mBright Text\e[0m"
    bright_bold="\e[${fg};1mBright Bold\e[0m"
    printf "%-16s %-30b %-30b\n" "$name" "$bright" "$bright_bold"
done

# 3. 16-Color Palette Grid
echo -e "\n--- 16-Color Palette Blocks ---"
echo -n "Normal (40-47): "
for bg in {40..47}; do
    printf "\e[%sm  %2s  \e[0m" "$bg" "$bg"
done
echo ""

echo -n "Bright (100-107):"
for bg in {100..107}; do
    printf "\e[%sm %3s  \e[0m" "$bg" "$bg"
done
echo -e "\n"

# 4. Color Combinations (Alerts / Badges)
echo "--- Combined Styles (Alerts & Tags) ---"
echo -e "  \e[41;37;1m ERROR \e[0m Connection refused on port 8080"
echo -e "  \e[42;30;1m SUCCESS \e[0m Build completed in 1.42s"
echo -e "  \e[43;30;1m WARN \e[0m Memory threshold exceeded: 85%"
echo -e "  \e[44;37;1m INFO \e[0m Listening on http://localhost:3000"
echo -e "  \e[45;37;1m DEBUG \e[0m Dispatched CSI sequence 'm'"
echo -e "  \e[46;30;1m NOTICE \e[0m Reloading configuration file"

# 5. Extended 256-color prefix (38;5;n / 48;5;n for first 16 colors)
echo -e "\n--- Extended SGR (38;5;n) Test ---"
for i in {0..15}; do
    printf "\e[38;5;%sm#%-2s \e[0m" "$i" "$i"
    if [ "$i" -eq 7 ]; then echo ""; fi
done
echo -e "\n"

echo "=== End of Test Suite ==="
