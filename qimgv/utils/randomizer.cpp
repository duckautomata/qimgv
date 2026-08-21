#include "randomizer.h"

Randomizer::Randomizer() {
    setCount(0);
}

Randomizer::Randomizer(int _count) {
    setCount(_count);
}

void Randomizer::setCount(int _count) {
    vec.resize(_count > 0 ? static_cast<size_t>(_count) : 0);
    fill();
    currentIndex = vec.empty() ? -1 : 0;
}

int Randomizer::count() const {
    return static_cast<int>(vec.size());
}

void Randomizer::shuffle() {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::shuffle(vec.begin(), vec.end(), rng);
}

void Randomizer::setCurrent(int _current) {
    currentIndex = indexOf(_current);
}

// Assumes the vector holds a shuffled permutation of [0 .. count), which it
// does in our case.
int Randomizer::indexOf(int item) const {
    if(item < 0 || item >= count())
        return -1;
    for(int i = 0; i < count(); i++) {
        if(vec[static_cast<size_t>(i)] == item)
            return i;
    }
    return -1;
}

void Randomizer::fill() {
    for(int i = 0; i < count(); i++)
        vec[static_cast<size_t>(i)] = i;
}

void Randomizer::print() {
    qDebug() << "---vector---";
    for(int v : vec)
        qDebug() << v;
    qDebug() << "----end----";
}

// Re-shuffles when we run off the end. Reshuffling rearranges the vector, so
// prev() is not meaningful across a wrap.
//
// Previously this was a `while(currentIndex == vec.size() - 1)` loop, which
// hung forever on a single-element vector (the item is always both first and
// last, so the condition never cleared) and read vec[-1] on an empty one --
// vec.size() - 1 underflows to SIZE_MAX for an unsigned size_t.
int Randomizer::next() {
    if(vec.empty())
        return -1;
    if(vec.size() == 1)
        return vec[0];

    if(currentIndex < 0) {
        currentIndex = 0;
        return vec[0];
    }
    if(currentIndex >= count() - 1) {
        const int currentItem = vec[static_cast<size_t>(currentIndex)];
        shuffle();
        setCurrent(currentItem);
        // The same item can land on the last slot again; rather than retrying
        // forever, just wrap to the start.
        if(currentIndex < 0 || currentIndex >= count() - 1) {
            currentIndex = 0;
            return vec[0];
        }
    }
    currentIndex++;
    return vec[static_cast<size_t>(currentIndex)];
}

int Randomizer::prev() {
    if(vec.empty())
        return -1;
    if(vec.size() == 1)
        return vec[0];

    if(currentIndex < 0) {
        currentIndex = count() - 1;
        return vec[static_cast<size_t>(currentIndex)];
    }
    if(currentIndex == 0) {
        const int currentItem = vec[0];
        shuffle();
        setCurrent(currentItem);
        if(currentIndex <= 0) {
            currentIndex = count() - 1;
            return vec[static_cast<size_t>(currentIndex)];
        }
    }
    currentIndex--;
    return vec[static_cast<size_t>(currentIndex)];
}
