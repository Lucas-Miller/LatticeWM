#include <windows.h>
#include <vector>
#include <iostream>
#include <cmath>

// Define MOD key (can be changed to MOD_CONTROL, MOD_WIN, etc.)
const UINT MOD_KEY = MOD_ALT;

// Structure to hold window information
struct WindowNode {
    HWND hwnd;
    RECT rect;         // Current position and size
    RECT savedRect;    // Saved original position and size for fullscreen toggle
    LONG savedStyle;   // Saved original style for fullscreen toggle
    WindowNode* left = nullptr;
    WindowNode* right = nullptr;
    bool isFullscreen = false; // Track fullscreen state
};

void MoveWindowNormalized(HWND hwnd, int x, int y, int width, int height) {
    if (!hwnd) return;

    // Retrieve original style
    LONG originalStyle = GetWindowLong(hwnd, GWL_STYLE);

    // Temporarily set to a normalized style
    SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // Move the window
    if (!SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOZORDER | SWP_SHOWWINDOW)) {
        std::cerr << "Failed to move window. Error: " << GetLastError() << "\n";
    }

    // Restore original style
    SetWindowLong(hwnd, GWL_STYLE, originalStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}


std::vector<WindowNode> layout; // Stores the tiled window layout

// Callback to collect visible windows that will be managed
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto windows = reinterpret_cast<std::vector<WindowNode>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE; // Skip invisible windows
    if (GetWindowTextLength(hwnd) == 0) return TRUE; // Skip untitled windows
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW) return TRUE; // Skip tool windows
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if ((style & WS_POPUP) || (style & WS_CHILD)) return TRUE; // Skip popups or child windows

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        std::cerr << "Failed to retrieve RECT for hwnd=" << hwnd << ". Error: " << GetLastError() << "\n";
        return TRUE;
    }

    if (rect.left == rect.right || rect.top == rect.bottom) return TRUE; // Skip windows with no area

    windows->push_back({ hwnd, rect });
    return TRUE;
}

// Link the windows as a doubly linked list
void LinkWindows(std::vector<WindowNode>& windows) {
    for (size_t i = 0; i < windows.size(); ++i) {
        if (i > 0) windows[i].left = &windows[i - 1];
        if (i < windows.size() - 1) windows[i].right = &windows[i + 1];
    }
}

// Swap positions of two adjacent windows
void SwapWindows(WindowNode* a, WindowNode* b) {
    if (!a || !b) return;

    std::cout << "Swapping windows: " << a->hwnd << " and " << b->hwnd << "\n";

    // Retrieve and validate the current window positions
    RECT rectA, rectB;
    if (!GetWindowRect(a->hwnd, &rectA) || !GetWindowRect(b->hwnd, &rectB)) {
        std::cerr << "Failed to get window rects. Error: " << GetLastError() << "\n";
        return;
    }

    // Swap positions using the normalized move function
    MoveWindowNormalized(a->hwnd, rectB.left, rectB.top,
                         rectB.right - rectB.left, rectB.bottom - rectB.top);
    MoveWindowNormalized(b->hwnd, rectA.left, rectA.top,
                         rectA.right - rectA.left, rectA.bottom - rectA.top);

    // Update their links in the layout
    WindowNode* tempLeft = a->left;
    WindowNode* tempRight = b->right;

    if (a->left) a->left->right = b;
    if (b->right) b->right->left = a;

    b->left = tempLeft;
    a->right = tempRight;

    a->left = b;
    b->right = a;

    std::cout << "Swap completed successfully.\n";
}


// Function to calculate and apply dimensions to windows
void TileWindows(std::vector<WindowNode>& windows) {
    int numWindows = static_cast<int>(windows.size());
    if (numWindows <= 0) {
        std::cerr << "No windows to tile.\n";
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    int screenWidth = GetDeviceCaps(hdcScreen, HORZRES);
    int screenHeight = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    int baseWidth = screenWidth / numWindows;  // Base width for each window
    int remainder = screenWidth % numWindows; // Remaining pixels to distribute
    int xOffset = 0;

    std::cout << "Screen dimensions: Width=" << screenWidth
              << ", Height=" << screenHeight << "\n";
    std::cout << "Base width: " << baseWidth << ", Remainder: " << remainder << "\n";

    for (int i = 0; i < numWindows; ++i) {
        // Calculate width for this window
        int width = baseWidth + (i < remainder ? 1 : 0);
        int height = screenHeight;

        std::cout << "Window " << i + 1 << " Target Position: xOffset=" << xOffset
                  << ", Width=" << width << ", Height=" << height << "\n";

        HWND hwnd = windows[i].hwnd;

        // Use MoveWindowNormalized for consistent handling
        MoveWindowNormalized(hwnd, xOffset, 0, width, height);

        // Log the actual dimensions after adjustment
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            std::cout << "Window " << i + 1 << " Actual RECT: Left=" << rect.left
                      << ", Right=" << rect.right
                      << ", Width=" << (rect.right - rect.left) << "\n";
        } else {
            std::cerr << "Failed to get RECT for window " << i + 1 << ". Error: " << GetLastError() << "\n";
        }

        // Update the offset for the next window
        xOffset += width;
    }

    std::cout << "Final Cumulative Offset: " << xOffset
              << ", Expected Screen Width: " << screenWidth << "\n";

    std::cout << "Windows tiled successfully.\n";
}



// Function to toggle fullscreen for a window
void SetWindowFullscreen(WindowNode* node) {
    if (!node) return;

    if (!node->isFullscreen) {
        // Save current window state
        node->savedStyle = GetWindowLong(node->hwnd, GWL_STYLE);
        if (!GetWindowRect(node->hwnd, &node->savedRect)) {
            std::cerr << "Failed to get window rect. Error: " << GetLastError() << "\n";
            return;
        }

        // Remove borders, title bar, etc.
        SetWindowLong(node->hwnd, GWL_STYLE, node->savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU));

        // Get screen dimensions
        HMONITOR hMonitor = MonitorFromWindow(node->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
            std::cerr << "Failed to get monitor info. Error: " << GetLastError() << "\n";
            return;
        }

        // Resize and reposition to cover the entire screen
        MoveWindowNormalized(node->hwnd,
                             monitorInfo.rcMonitor.left,
                             monitorInfo.rcMonitor.top,
                             monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                             monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
    } else {
        // Restore original window style
        SetWindowLong(node->hwnd, GWL_STYLE, node->savedStyle);
        SetWindowPos(node->hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        // Restore original window size and position
        MoveWindowNormalized(node->hwnd,
                             node->savedRect.left,
                             node->savedRect.top,
                             node->savedRect.right - node->savedRect.left,
                             node->savedRect.bottom - node->savedRect.top);
    }

    // Toggle the fullscreen flag
    node->isFullscreen = !node->isFullscreen;

    // Force redraw
    ShowWindow(node->hwnd, SW_SHOW);
}


// Function to focus a window
void FocusWindow(WindowNode* current, bool left) {
    if (!current) return;

    WindowNode* target = left ? current->left : current->right;

    if (target) {
        char title[256];
        GetWindowTextA(target->hwnd, title, sizeof(title));
        std::cout << "Focusing window: " << title << "\n";

        ShowWindow(target->hwnd, SW_RESTORE);
        SetWindowPos(target->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(target->hwnd);
    } else {
        std::cout << "No window in the specified direction.\n";
    }
}

// Function to register hotkeys
bool RegisterHotKeys() {
    if (!RegisterHotKey(nullptr, 1, MOD_KEY, VK_LEFT)) return false;
    if (!RegisterHotKey(nullptr, 2, MOD_KEY, VK_RIGHT)) return false;
    if (!RegisterHotKey(nullptr, 3, MOD_KEY, 'F')) return false;
    if (!RegisterHotKey(nullptr, 4, MOD_KEY | MOD_SHIFT, VK_LEFT)) return false; // Swap left
    if (!RegisterHotKey(nullptr, 5, MOD_KEY | MOD_SHIFT, VK_RIGHT)) return false; // Swap right
    return true;
}

int main() {
    // Enumerate all visible windows
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&layout));

    // Link windows into a doubly linked list
    LinkWindows(layout);

    // Tile the windows
    TileWindows(layout);

    // Register hotkeys for switching windows
    if (!RegisterHotKeys()) {
        std::cerr << "Failed to register hotkeys.\n";
        return 1;
    }

    // Message loop to handle hotkey events
    MSG msg = { 0 };
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            HWND current = GetForegroundWindow();
            WindowNode* currentNode = nullptr;

            for (auto& node : layout) {
                if (node.hwnd == current) {
                    currentNode = &node;
                    break;
                }
            }

            if (currentNode) {
                switch (msg.wParam) {
                    case 1:
                        FocusWindow(currentNode, true);  // MOD + LEFT
                        break;
                    case 2:
                        FocusWindow(currentNode, false); // MOD + RIGHT
                        break;
                    case 3:
                        SetWindowFullscreen(currentNode); // MOD + F
                        break;
                    case 4:
                        if (currentNode->left) SwapWindows(currentNode->left, currentNode); // MOD + SHIFT + LEFT
                        break;
                    case 5:
                        if (currentNode->right) SwapWindows(currentNode, currentNode->right); // MOD + SHIFT + RIGHT
                        break;
                }
            }
        }
    }

    return 0;
}
