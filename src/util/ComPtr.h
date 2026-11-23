#pragma once

#include <unknwn.h>
#include <cstddef>
#include <utility>

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}

    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ~ComPtr() noexcept {
        Reset();
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            ComPtr tmp(other);
            Swap(tmp);
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Swap(ComPtr& other) noexcept {
        std::swap(ptr_, other.ptr_);
    }

    void Reset() noexcept {
        if (ptr_) {
            T* tmp = ptr_;
            ptr_ = nullptr;
            tmp->Release();
        }
    }

    void Attach(T* ptr) noexcept {
        if (ptr_ != ptr) {
            Reset();
            ptr_ = ptr;
        }
    }

    T* Detach() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** GetAddressOf() noexcept { return &ptr_; }

    T** ReleaseAndGetAddressOf() noexcept {
        Reset();
        return &ptr_;
    }

    template <typename U>
    HRESULT As(ComPtr<U>* out) const noexcept {
        if (!out) return E_POINTER;
        out->Reset();
        if (!ptr_) return E_NOINTERFACE;
        return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
    }

    template <typename U>
    HRESULT As(REFIID riid, ComPtr<U>* out) const noexcept {
        if (!out) return E_POINTER;
        out->Reset();
        if (!ptr_) return E_NOINTERFACE;
        return ptr_->QueryInterface(riid, reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
    }

private:
    T* ptr_ = nullptr;
};
