#include <iostream>
#include <vector>
#include <algorithm>
#include <sstream>

namespace sort_lib {
void quicksort(int arr[], int left, int right) {
    if (left >= right)
        return;
    int pivot = arr[right];
    int i = left - 1;
    for (int j = left; j < right; ++j) {
        if (arr[j] < pivot) {
            ++i;
            std::swap(arr[i], arr[j]);
        }
    }
    std::swap(arr[i + 1], arr[right]);
    int pi = i + 1;
    quicksort(arr, left, pi - 1);
    quicksort(arr, pi + 1, right);
}
void quicksort(std::vector<int> &v) {
    if (v.empty())
        return;
    quicksort(v.data(), 0, (int)v.size() - 1);
}
std::string quicksort_str(const std::vector<int> &v) {
    std::vector<int> arr = v;
    quicksort(arr);
    std::ostringstream oss;
    for (size_t i = 0; i < arr.size(); ++i)
        oss << arr[i] << (i == arr.size() - 1 ? "" : ", ");
    return oss.str();
}
} // namespace sort_lib

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    std::vector<int> v;
    int N;
    if (std::cin >> N) {
        v.resize(N);
        for (int i = 0; i < N; ++i)
            std::cin >> v[i];
    } else {
        std::cerr << "No input\n";
        return 1;
    }
    sort_lib::quicksort(v);
    // std::cout << N << "\n";
    for (size_t i = 0; i < v.size(); ++i) {
        std::cout << v[i] << ' ';
    }
    std::cout << "\n";
    return 0;
}
