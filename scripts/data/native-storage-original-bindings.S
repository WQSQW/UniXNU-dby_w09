// SPDX-License-Identifier: BSD-2-Clause-Patent
// Absolute addresses of the original IOStorageFamily symbols a source block driver imports.
//
// Source: out/ipsw/21H461/kernelcache.decompressed, sha256
// f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7, the kernel this board
// is pinned to in profiles/os/ipados-17.7.11-21H461/profile.toml.
//
// Derivation (scripts/audit-storage-interface.py performs the same static steps):
//   1. Compile refer/IOStorageFamily/IOBlockStorageDevice.cpp, read its dispatch table from
//      the object's relocations: 190 methods, 14 of them __cxa_pure_virtual.
//   2. Scan the kext's __DATA_CONST for the one table whose pointers match every method name
//      that the kernel exports: 176 kernel bindings, 14 addresses inside the kext's own text.
//   3. Take from that table the destructor, the constructor, the metaclass getter and the
//      methods the source class overrides.
//   4. IOMedia derives from IOStorage, and IOFilterScheme does too. Of the three tables that
//      match the IOMedia shape, only one has exactly IOMedia's own override set relative to
//      IOStorage's table; its metaclass getter returns the IOMedia metaclass.
//
// These are original link addresses. The kernel build rebases the prelinked components by
// -0x1000000, so each value here is the original address minus 0x1000000.

.globl __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass
.set __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass, 0xfffffff00496ed98
.globl __ZN20IOBlockStorageDeviceD2Ev
.set __ZN20IOBlockStorageDeviceD2Ev, 0xfffffff00496edb8
.globl __ZN20IOBlockStorageDevice10gMetaClassE
.set __ZN20IOBlockStorageDevice10gMetaClassE, 0xfffffff006add1d0
.globl __ZTV20IOBlockStorageDevice
.set __ZTV20IOBlockStorageDevice, 0xfffffff005cb44c0
.globl __ZN20IOBlockStorageDevice4initEP12OSDictionary
.set __ZN20IOBlockStorageDevice4initEP12OSDictionary, 0xfffffff00496ee30
.globl __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol
.set __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol, 0xfffffff00496ee88
.globl __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject
.set __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject, 0xfffffff00496ef38
.globl __ZN20IOBlockStorageDevice11requestIdleEv
.set __ZN20IOBlockStorageDevice11requestIdleEv, 0xfffffff00496f050
.globl __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj
.set __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj, 0xfffffff00496f05c
.globl __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh
.set __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh, 0xfffffff00496f068
.globl __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty
.set __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty, 0xfffffff00496f08c
// The metaclass pointer of IOMedia, used by OSDynamicCast (IOMedia, ...) in the driver.
.globl __ZN7IOMedia9metaClassE
.set __ZN7IOMedia9metaClassE, 0xfffffff005cb6a70
