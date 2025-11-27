#!/bin/bash
# Install kiro-parallel to /usr/local/bin

SCRIPT_CONTENT='#!/bin/bash
# kiro-parallel - Wrapper script for parallel task orchestration
# Locates kiro_parallel.py in the current project and runs it

SCRIPT_NAME="kiro_parallel.py"
SCRIPT_PATHS=(
    "tools/kiro-parallel/${SCRIPT_NAME}"
    ".kiro/tools/kiro-parallel/${SCRIPT_NAME}"
    "scripts/kiro-parallel/${SCRIPT_NAME}"
)

# Find the script in the project
find_script() {
    for path in "${SCRIPT_PATHS[@]}"; do
        if [[ -f "$path" ]]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

# Main
SCRIPT_PATH=$(find_script)

if [[ -z "$SCRIPT_PATH" ]]; then
    echo "Error: Could not find ${SCRIPT_NAME} in any of the expected locations:"
    for path in "${SCRIPT_PATHS[@]}"; do
        echo "  - $path"
    done
    echo ""
    echo "Make sure you are in a project root directory with kiro-parallel installed."
    exit 1
fi

# Run the Python script with all arguments
exec python3 "$SCRIPT_PATH" "$@"
'

echo "Installing kiro-parallel to /usr/local/bin..."
echo "$SCRIPT_CONTENT" | sudo tee /usr/local/bin/kiro-parallel > /dev/null
sudo chmod +x /usr/local/bin/kiro-parallel
echo "Done! You can now use 'kiro-parallel' from any project directory."
