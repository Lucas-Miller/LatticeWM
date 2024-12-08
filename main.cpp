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

// Function to calculate and apply dimensions to windows
void TileWindows(std::vector<WindowNode>& windows) {
    int numWindows = static_cast<int>(windows.size());
    if (numWindows <= 0) {
        std::cerr << "No windows to tile.\n";
        return;
    }

    // Get accurate screen width and height
    HDC hdcScreen = GetDC(nullptr);
    int screenWidth = GetDeviceCaps(hdcScreen, HORZRES);
    int screenHeight = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    std::cout << "Screen dimensions: Width=" << screenWidth << ", Height=" << screenHeight << "\n";
    std::cout << "Number of windows: " << numWindows << "\n";

    float cumulativeOffset = 0.0f;

    for (int i = 0; i < numWindows; ++i) {
        // Proportional width calculation
        float startOffset = cumulativeOffset;
        cumulativeOffset += static_cast<float>(screenWidth) / numWindows;

        int desiredWidth = static_cast<int>(std::round(cumulativeOffset)) - static_cast<int>(std::round(startOffset));
        int desiredHeight = screenHeight;

        HWND hwnd = windows[i].hwnd;

        // Adjust for non-client area (borders, title bar, etc.)
        RECT adjustedRect = { 0, 0, desiredWidth, desiredHeight };
        if (!AdjustWindowRect(&adjustedRect, GetWindowLong(hwnd, GWL_STYLE), FALSE)) {
            std::cerr << "Failed to adjust window rect for window " << i + 1 << ". Error: " << GetLastError() << "\n";
            continue;
        }

        int actualWidth = adjustedRect.right - adjustedRect.left;
        int actualHeight = adjustedRect.bottom - adjustedRect.top;

        // Correct offset to avoid gaps or overlaps
        int xOffset = static_cast<int>(std::round(startOffset));

        std::cout << "Tiling Window " << i + 1 << ": xOffset=" << xOffset
                  << ", Desired Width=" << desiredWidth << ", Desired Height=" << desiredHeight
                  << ", Adjusted Width=" << actualWidth << ", Adjusted Height=" << actualHeight << "\n";

        // Restore window to normal state
        ShowWindow(hwnd, SW_SHOWNORMAL);

        // Set position and adjusted size
        if (!SetWindowPos(hwnd, HWND_TOP, xOffset, 0, actualWidth, actualHeight, SWP_NOZORDER | SWP_SHOWWINDOW)) {
            std::cerr << "Failed to set position for window " << i + 1 << ". Error: " << GetLastError() << "\n";
        }

        // Log actual dimensions after positioning
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            std::cout << "Window " << i + 1 << " Actual RECT: Left=" << rect.left
                      << ", Top=" << rect.top
                      << ", Right=" << rect.right
                      << ", Bottom=" << rect.bottom
                      << ", Width=" << (rect.right - rect.left)
                      << ", Height=" << (rect.bottom - rect.top) << "\n";
        } else {
            std::cerr << "Failed to get RECT for window " << i + 1 << ". Error: " << GetLastError() << "\n";
        }

        
        cumulativeOffset = static_cast<float>(rect.right);
    }

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
        SetWindowPos(node->hwnd, HWND_TOP, 
                     monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top, 
                     monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left, 
                     monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top, 
                     SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        // Restore original window style
        SetWindowLong(node->hwnd, GWL_STYLE, node->savedStyle);

        // Restore original window size and position
        SetWindowPos(node->hwnd, HWND_TOP, 
                     node->savedRect.left, node->savedRect.top, 
                     node->savedRect.right - node->savedRect.left, 
                     node->savedRect.bottom - node->savedRect.top, 
                     SWP_NOZORDER | SWP_FRAMECHANGED);
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
                }
            }
        }
    }

    return 0;
}
