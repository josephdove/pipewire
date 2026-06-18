// Copyright (C) 2025, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

extern crate self as libc;
extern crate self as log;
extern crate self as zerocopy;

pub type c_int = i32;
pub type c_uint = u32;
pub type c_uchar = u8;
pub type c_ushort = u16;
pub type c_ulong = u64;
pub type c_longlong = i64;
pub type c_double = f64;

#[macro_export]
macro_rules! debug {
    ($($arg:tt)*) => {{}};
}

#[macro_export]
macro_rules! error {
    ($($arg:tt)*) => {{}};
}

#[macro_export]
macro_rules! info {
    ($($arg:tt)*) => {{}};
}

#[macro_export]
macro_rules! warn {
    ($($arg:tt)*) => {{}};
}

pub trait Immutable {}

pub trait IntoBytes {
    fn as_bytes(&self) -> &[u8];
    fn as_mut_bytes(&mut self) -> &mut [u8];
}

impl<T> IntoBytes for T {
    fn as_bytes(&self) -> &[u8] {
        unsafe {
            std::slice::from_raw_parts(
                (self as *const T).cast::<u8>(),
                std::mem::size_of::<T>(),
            )
        }
    }

    fn as_mut_bytes(&mut self) -> &mut [u8] {
        unsafe {
            std::slice::from_raw_parts_mut(
                (self as *mut T).cast::<u8>(),
                std::mem::size_of::<T>(),
            )
        }
    }
}

impl<T> IntoBytes for [T] {
    fn as_bytes(&self) -> &[u8] {
        unsafe {
            std::slice::from_raw_parts(
                self.as_ptr().cast::<u8>(),
                std::mem::size_of_val(self),
            )
        }
    }

    fn as_mut_bytes(&mut self) -> &mut [u8] {
        unsafe {
            std::slice::from_raw_parts_mut(
                self.as_mut_ptr().cast::<u8>(),
                std::mem::size_of_val(self),
            )
        }
    }
}

pub trait FromBytes {
    fn ref_from_prefix(bytes: &[u8]) -> Option<(&Self, &[u8])>
    where
        Self: Sized;
    fn ref_from_bytes(bytes: &[u8]) -> Option<&Self>;
    fn mut_from_bytes(bytes: &mut [u8]) -> Option<&mut Self>;
}

impl<T> FromBytes for T {
    fn ref_from_prefix(bytes: &[u8]) -> Option<(&Self, &[u8])> {
        let size = std::mem::size_of::<T>();
        if bytes.len() < size || (bytes.as_ptr() as usize) % std::mem::align_of::<T>() != 0 {
            return None;
        }
        let (head, tail) = bytes.split_at(size);
        Some((unsafe { &*head.as_ptr().cast::<T>() }, tail))
    }

    fn ref_from_bytes(bytes: &[u8]) -> Option<&Self> {
        let size = std::mem::size_of::<T>();
        if bytes.len() != size || (bytes.as_ptr() as usize) % std::mem::align_of::<T>() != 0 {
            return None;
        }
        Some(unsafe { &*bytes.as_ptr().cast::<T>() })
    }

    fn mut_from_bytes(bytes: &mut [u8]) -> Option<&mut Self> {
        let size = std::mem::size_of::<T>();
        if bytes.len() != size || (bytes.as_ptr() as usize) % std::mem::align_of::<T>() != 0 {
            return None;
        }
        Some(unsafe { &mut *bytes.as_mut_ptr().cast::<T>() })
    }
}

impl<T> FromBytes for [T] {
    fn ref_from_prefix(_bytes: &[u8]) -> Option<(&Self, &[u8])>
    where
        Self: Sized,
    {
        unreachable!()
    }

    fn ref_from_bytes(bytes: &[u8]) -> Option<&Self> {
        let size = std::mem::size_of::<T>();
        if size == 0 ||
            bytes.len() % size != 0 ||
            (bytes.as_ptr() as usize) % std::mem::align_of::<T>() != 0
        {
            return None;
        }
        Some(unsafe {
            std::slice::from_raw_parts(bytes.as_ptr().cast::<T>(), bytes.len() / size)
        })
    }

    fn mut_from_bytes(bytes: &mut [u8]) -> Option<&mut Self> {
        let size = std::mem::size_of::<T>();
        if size == 0 ||
            bytes.len() % size != 0 ||
            (bytes.as_ptr() as usize) % std::mem::align_of::<T>() != 0
        {
            return None;
        }
        Some(unsafe {
            std::slice::from_raw_parts_mut(bytes.as_mut_ptr().cast::<T>(), bytes.len() / size)
        })
    }
}

pub mod common {
    pub mod lhdc_level;
}

mod kiss_fft;
pub mod lhdc_enc {
    pub mod lhdc_enc_freq_process;
    pub mod lhdc_enc_header;
    pub mod lhdc_enc_workspace;
}

pub mod lhdc_api;

mod arith;
mod bits;
pub mod enc;
mod ffi;
mod math;

/// Inits logging for Android
#[cfg(target_os = "android")]
pub fn init_logging() {
    android_logger::init_once(android_logger::Config::default().with_tag("bluetooth"));
}

/// Inits logging for host
#[cfg(not(target_os = "android"))]
pub fn init_logging() {
}
