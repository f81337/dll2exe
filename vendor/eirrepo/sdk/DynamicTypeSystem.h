/*****************************************************************************
*
*  PROJECT:     Eir SDK
*  LICENSE:     See LICENSE in the top level directory
*  FILE:        eirrepo/sdk/DynamicTypeSystem.h
*  PURPOSE:     Dynamic runtime type abstraction system.
*
*  Find the Eir SDK at: https://osdn.net/projects/eirrepo/
*  Multi Theft Auto is available from http://www.multitheftauto.com/
*
*****************************************************************************/

#ifndef _DYN_TYPE_ABSTRACTION_SYSTEM_
#define _DYN_TYPE_ABSTRACTION_SYSTEM_

#include <string.h>

#include "rwlist.hpp"
#include "PluginFactory.h"

#include "eirutils.h"
#include "String.h"
#include "MetaHelpers.h"

// Type sentry struct of the dynamic type system.
// It notes the programmer that the struct has RTTI.
// Every type constructed through DynamicTypeSystem has this struct before it.
struct GenericRTTI
{
#ifdef _DEBUG
    void *typesys_ptr;      // pointer to the DynamicTypeSystem manager (for debugging)
#endif //_DEBUG
    void *type_meta;        // pointer to the struct that denotes the runtime type
};

// Assertions that can be thrown by the type system
#ifndef rtti_assert
#include <assert.h>

#define rtti_assert( x )    assert( x )
#endif //rtti_assert

struct dtsDefaultLockProvider
{
    typedef void rwlock;

    inline rwlock* CreateLock( void ) const
    {
        return nullptr;
    }

    inline void CloseLock( rwlock *lock ) const
    {
        // noop.
    }

    inline void LockEnterRead( rwlock *lock ) const
    {
        // noop.
    }

    inline void LockLeaveRead( rwlock *lock ) const
    {
        // noop.
    }

    inline void LockEnterWrite( rwlock *lock ) const
    {
        // noop.
    }

    inline void LockLeaveWrite( rwlock *lock ) const
    {
        // noop.
    }
};

// This class manages runtime type information.
// It allows for dynamic C++ class extension depending on runtime conditions.
// Main purpose for its creation are tight memory requirements.
#define DTS_TEMPLARGS \
    template <typename allocatorType, typename systemPointer, typename lockProvider_t = dtsDefaultLockProvider, template <typename genericType> typename structFlavorDispatchTypeTemplate = cachedMinimalStructRegistryFlavor>
#define DTS_TEMPLUSE \
    <allocatorType, systemPointer, lockProvider_t, structFlavorDispatchTypeTemplate>

DTS_TEMPLARGS
struct DynamicTypeSystem
{
    // REQUIREMENTS FOR allocatorType STRUCT INTERFACE:
    // - void* Allocate( size_t memSize )
    // - void Free( void *memPtr, size_t memSize )

private:
    typedef structFlavorDispatchTypeTemplate <GenericRTTI> structFlavorDispatchType;

public:
    typedef allocatorType memAllocType;

    static constexpr unsigned int ANONYMOUS_PLUGIN_ID = 0xFFFFFFFF;

    // TODO: actually change this to be dynamic!
    static constexpr size_t STANDARD_OBJECT_ALIGNMENT = sizeof(void*);

    // Export the system type so extensions can use it.
    typedef systemPointer systemPointer_t;

    // Exceptions thrown by this system.
    class abstraction_construction_exception    {};
    class type_name_conflict_exception          {};
    class undefined_method_exception            {};

    // Lock provider for MT support.
    typedef typename lockProvider_t::rwlock rwlock;

    lockProvider_t lockProvider;

    inline DynamicTypeSystem( void ) noexcept
    {
        this->mainLock = nullptr;
    }

    inline void Shutdown( void )
    {
        // Clean up our memory by deleting all types.
        while ( !LIST_EMPTY( registeredTypes.root ) )
        {
            typeInfoBase *info = LIST_GETITEM( typeInfoBase, registeredTypes.root.next, node );

            DeleteType( info );
        }

        // Remove our lock.
        if ( rwlock *sysLock = this->mainLock )
        {
            this->lockProvider.CloseLock( sysLock );

            this->mainLock = nullptr;
        }
    }

    inline ~DynamicTypeSystem( void )
    {
        Shutdown();
    }

    // Call this method once you have set up the "lockProvider" variable.
    // OTHERWISE THIS TYPE SYSTEM IS NOT THREAD-SAFE!
    inline void InitializeLockProvider( void )
    {
        this->mainLock = this->lockProvider.CreateLock();
    }

private:
    // Lock used when using fields of the type system itself.
    rwlock *mainLock;

protected:
    struct scoped_rwlock_read
    {
        scoped_rwlock_read( const scoped_rwlock_read& ) = delete;

        inline scoped_rwlock_read( const lockProvider_t& provider, rwlock *theLock ) : lockProvider( provider )
        {
            if ( theLock )
            {
                provider.LockEnterRead( theLock );
            }

            this->theLock = theLock;
        }

        inline ~scoped_rwlock_read( void )
        {
            if ( rwlock *theLock = this->theLock )
            {
                lockProvider.LockLeaveRead( theLock );

                this->theLock = nullptr;
            }
        }

    private:
        const lockProvider_t& lockProvider;
        rwlock *theLock;
    };

    struct scoped_rwlock_write
    {
        scoped_rwlock_write( const scoped_rwlock_write& ) = delete;

        inline scoped_rwlock_write( const lockProvider_t& provider, rwlock *theLock ) : lockProvider( provider )
        {
            if ( theLock )
            {
                provider.LockEnterWrite( theLock );
            }

            this->theLock = theLock;
        }

        inline ~scoped_rwlock_write( void )
        {
            if ( rwlock *theLock = this->theLock )
            {
                lockProvider.LockLeaveWrite( theLock );

                this->theLock = nullptr;
            }
        }

    private:
        const lockProvider_t& lockProvider;
        rwlock *theLock;
    };

public:
    // Interface for type lifetime management.
    // THIS INTERFACE MUST BE THREAD-SAFE! This means that it has to provided its own locks, when necessary!
    struct typeInterface abstract
    {
        virtual void Construct( void *mem, systemPointer_t *sysPtr, void *construct_params ) const = 0;
        virtual void CopyConstruct( void *mem, const void *srcMem ) const = 0;
        virtual void Destruct( void *mem ) const = 0;

        // The type size of objects is assumed to be an IMMUTABLE property.
        // Changing the type size of an object leads to undefined behavior.
        virtual size_t GetTypeSize( systemPointer_t *sysPtr, void *construct_params ) const = 0;

        virtual size_t GetTypeSizeByObject( systemPointer_t *sysPtr, const void *mem ) const = 0;
    };

    struct typeInfoBase;

    // THREAD-SAFE, because it is an IMMUTABLE struct.
    struct pluginDescriptor
    {
        friend struct DynamicTypeSystem;

        typedef ptrdiff_t pluginOffset_t;

        inline pluginDescriptor( void )
        {
            this->pluginId = DynamicTypeSystem::ANONYMOUS_PLUGIN_ID;
            this->typeInfo = nullptr;
        }

        inline pluginDescriptor( unsigned int id, typeInfoBase *typeInfo )
        {
            this->pluginId = id;
            this->typeInfo = typeInfo;
        }

    private:
        // Plugin descriptors are immutable beyond construction.
        unsigned int pluginId;
        typeInfoBase *typeInfo;

    public:
        template <typename pluginStructType>
        AINLINE pluginStructType* RESOLVE_STRUCT( GenericRTTI *object, pluginOffset_t offset, systemPointer_t *sysPtr )
        {
            return DynamicTypeSystem::RESOLVE_STRUCT <pluginStructType> ( sysPtr, object, this->typeInfo, offset );
        }

        template <typename pluginStructType>
        AINLINE const pluginStructType* RESOLVE_STRUCT( const GenericRTTI *object, pluginOffset_t offset, systemPointer_t *sysPtr )
        {
            return DynamicTypeSystem::RESOLVE_STRUCT <const pluginStructType> ( sysPtr, object, this->typeInfo, offset );
        }
    };

private:
    // Need to redirect the typeInfoBase structRegistry allocations to the DynamicTypeSystem allocator.
    DEFINE_HEAP_REDIR_ALLOC( structRegRedirAlloc );

public:
    typedef AnonymousPluginStructRegistry <GenericRTTI, pluginDescriptor, structFlavorDispatchType, structRegRedirAlloc, systemPointer_t*> structRegistry_t;

    // Localize important struct details.
    typedef typename pluginDescriptor::pluginOffset_t pluginOffset_t;
    typedef typename structRegistry_t::pluginInterface pluginInterface;

    static const pluginOffset_t INVALID_PLUGIN_OFFSET = (pluginOffset_t)-1;

    // THREAD-SAFE, only as long as it is used only through DynamicTypeSystem!
    struct typeInfoBase abstract
    {
        inline typeInfoBase( void )
        { }

        virtual ~typeInfoBase( void )
        { }

        typeInfoBase( const typeInfoBase& right ) = delete;

        // Helper for the Cleanup method.
        typedef allocatorType typeInfoAllocatorType;

        virtual void Cleanup( void ) = 0;

        DynamicTypeSystem *typeSys;

        const char *name;   // name of this type
        typeInterface *tInterface;  // type construction information

        volatile unsigned long refCount;    // number of entities that use this type

        // WARNING: as long as a type is referenced, it MUST not change!!!
        inline bool IsImmutable( void ) const
        {
            return ( this->refCount != 0 );
        }

        unsigned long inheritanceCount; // number of types inheriting from this type

        inline bool IsEndType( void ) const
        {
            return ( this->inheritanceCount == 0 );
        }

        // Special properties.
        bool isExclusive;       // can be used by the runtime to control the creation of objects.
        bool isAbstract;        // can this type be constructed (set internally)

        // Inheritance information.
        typeInfoBase *inheritsFrom; // type that this type inherits from

        // Plugin information.
        structRegistry_t structRegistry;

        // Lock used when accessing this type itself.
        rwlock *typeLock;

        RwListEntry <typeInfoBase> node;
    };

    AINLINE static size_t GetTypePluginOffset( const GenericRTTI *rtObj, typeInfoBase *subclassTypeInfo, typeInfoBase *offsetInfo )
    {
        size_t offset = 0;

        DynamicTypeSystem *typeSys = subclassTypeInfo->typeSys;

        if ( typeInfoBase *inheritsFrom = offsetInfo->inheritsFrom )
        {
            offset += GetTypePluginOffset( rtObj, subclassTypeInfo, inheritsFrom );

            scoped_rwlock_read lock( typeSys->lockProvider, inheritsFrom->typeLock );

            offset += inheritsFrom->structRegistry.GetPluginSizeByObject( rtObj );
        }

        return offset;
    }

    AINLINE static typeInfoBase* GetTypeInfoFromTypeStruct( const GenericRTTI *rtObj )
    {
        return (typeInfoBase*)rtObj->type_meta;
    }

    // Function to get the offset of a type information on an object.
    AINLINE static pluginOffset_t GetTypeInfoStructOffset( systemPointer_t *sysPtr, GenericRTTI *rtObj, typeInfoBase *offsetInfo )
    {
        // This method is thread safe, because every operation is based on immutable data or is atomic already.

        pluginOffset_t baseOffset = 0;

        // Get generic type information.
        typeInfoBase *subclassTypeInfo = GetTypeInfoFromTypeStruct( rtObj );

        // Get the pointer to the language object.
        void *langObj = GetObjectFromTypeStruct( rtObj );

        baseOffset += sizeof( GenericRTTI );

        // Add the dynamic size of the language object.
        baseOffset += subclassTypeInfo->tInterface->GetTypeSizeByObject( sysPtr, langObj );

        // Add the plugin offset.
        {
            DynamicTypeSystem *typeSys = subclassTypeInfo->typeSys;

            scoped_rwlock_read baseLock( typeSys->lockProvider, subclassTypeInfo->typeLock );

            baseOffset += GetTypePluginOffset( rtObj, subclassTypeInfo, offsetInfo );
        }

        return baseOffset;
    }

    AINLINE static bool IsOffsetValid( pluginOffset_t offset )
    {
        return ( offset != INVALID_PLUGIN_OFFSET );
    }

    // Struct resolution methods are thread safe.
    template <typename pluginStructType>
    AINLINE static pluginStructType* RESOLVE_STRUCT( systemPointer_t *sysPtr, GenericRTTI *rtObj, typeInfoBase *typeInfo, pluginOffset_t offset )
    {
        if ( !IsOffsetValid( offset ) )
            return nullptr;

        size_t baseOffset = GetTypeInfoStructOffset( sysPtr, rtObj, typeInfo );

        pluginOffset_t realOffset = GetTypeRegisteredPluginLocation( typeInfo, rtObj, offset );

        return (pluginStructType*)( (char*)rtObj + baseOffset + realOffset );
    }

    template <typename pluginStructType>
    AINLINE static const pluginStructType* RESOLVE_STRUCT( systemPointer_t *sysPtr, const GenericRTTI *rtObj, typeInfoBase *typeInfo, pluginOffset_t offset )
    {
        if ( !IsOffsetValid( offset ) )
            return nullptr;

        size_t baseOffset = GetTypeInfoStructOffset( sysPtr, (GenericRTTI*)rtObj, typeInfo );

        pluginOffset_t realOffset = GetTypeRegisteredPluginLocation( typeInfo, rtObj, offset );

        return (const pluginStructType*)( (char*)rtObj + baseOffset + realOffset );
    }

    // Function used to register a new plugin struct into the class.
    inline pluginOffset_t RegisterPlugin( size_t pluginSize, const pluginDescriptor& descriptor, pluginInterface *plugInterface )
    {
        scoped_rwlock_write lock( this->lockProvider, descriptor.typeInfo->typeLock );

        rtti_assert( descriptor.typeInfo->IsImmutable() == false );

        return descriptor.typeInfo->structRegistry.RegisterPlugin( pluginSize, descriptor, plugInterface );
    }

    // Function used to register custom plugins whose lifetime is managed by this container.
    template <typename structTypeInterface, typename... Args>
    inline pluginOffset_t RegisterCustomPlugin( size_t pluginSize, const pluginDescriptor& descriptor, Args&&... constrArgs )
    {
        struct selfDestroyingInterface : public structTypeInterface
        {
            inline selfDestroyingInterface( DynamicTypeSystem *typeSys, Args&&... constrArgs ) : structTypeInterface( std::forward <Args> ( constrArgs )... )
            {
                this->typeSys = typeSys;
            }

            void DeleteOnUnregister( void ) override
            {
                eir::static_del_struct <selfDestroyingInterface, allocatorType> ( this->typeSys, this );
            }

            DynamicTypeSystem *typeSys;
        };

        selfDestroyingInterface *pluginInfo = eir::static_new_struct <selfDestroyingInterface, allocatorType> ( this, this, std::forward <Args> ( constrArgs )... );

        try
        {
            return RegisterPlugin( pluginSize, descriptor, pluginInfo );
        }
        catch( ... )
        {
            eir::static_del_struct <selfDestroyingInterface, allocatorType> ( this, pluginInfo );

            throw;
        }
    }

    inline void UnregisterPlugin( typeInfoBase *typeInfo, pluginOffset_t pluginOffset )
    {
        scoped_rwlock_write lock( this->lockProvider, typeInfo->typeLock );

        rtti_assert( typeInfo->IsImmutable() == false );

        typeInfo->structRegistry.UnregisterPlugin( pluginOffset );
    }

    // Plugin registration functions.
    typedef CommonPluginSystemDispatch <GenericRTTI, DynamicTypeSystem, pluginDescriptor, systemPointer_t*> functoidHelper_t;

    template <typename structType>
    inline pluginOffset_t RegisterStructPlugin( typeInfoBase *typeInfo, unsigned int pluginId = ANONYMOUS_PLUGIN_ID )
    {
        pluginDescriptor descriptor( pluginId, typeInfo );

        return functoidHelper_t( *this ).template RegisterStructPlugin <structType> ( descriptor );
    }

    template <typename structType>
    inline pluginOffset_t RegisterDependantStructPlugin( typeInfoBase *typeInfo, unsigned int pluginId = ANONYMOUS_PLUGIN_ID, size_t structSize = sizeof(structType) )
    {
        pluginDescriptor descriptor( pluginId, typeInfo );

        return functoidHelper_t( *this ).template RegisterDependantStructPlugin <structType> ( descriptor, structSize );
    }

    typedef typename functoidHelper_t::conditionalPluginStructInterface conditionalPluginStructInterface;

    template <typename structType>
    inline pluginOffset_t RegisterDependantConditionalStructPlugin( typeInfoBase *typeInfo, unsigned int pluginId, conditionalPluginStructInterface *conditional, size_t structSize = sizeof(structType) )
    {
        pluginDescriptor descriptor( pluginId, typeInfo );

        return functoidHelper_t( *this ).template RegisterDependantConditionalStructPlugin <structType> ( descriptor, conditional, structSize );
    }

    RwList <typeInfoBase> registeredTypes;

    inline void SetupTypeInfoBase( typeInfoBase *tInfo, const char *typeName, typeInterface *tInterface, typeInfoBase *inheritsFrom )
    {
        scoped_rwlock_write lock( this->lockProvider, this->mainLock );

        // If we find a type info with this name already, throw an exception.
        {
            typeInfoBase *alreadyExisting = FindTypeInfoNolock( typeName, inheritsFrom );

            if ( alreadyExisting != nullptr )
            {
                throw type_name_conflict_exception();
            }
        }

        tInfo->typeSys = this;
        tInfo->name = typeName;
        tInfo->refCount = 0;
        tInfo->inheritanceCount = 0;
        tInfo->isExclusive = false;
        tInfo->isAbstract = false;
        tInfo->tInterface = tInterface;
        tInfo->inheritsFrom = nullptr;
        tInfo->typeLock = lockProvider.CreateLock();
        LIST_INSERT( registeredTypes.root, tInfo->node );

        // Set inheritance.
        try
        {
            this->SetTypeInfoInheritingClass( tInfo, inheritsFrom, false );
        }
        catch( ... )
        {
            if ( tInfo->typeLock )
            {
                lockProvider.CloseLock( tInfo->typeLock );
            }

            LIST_REMOVE( tInfo->node );

            throw;
        }
    }

    // Already THREAD-SAFE, because memory allocation is THREAD-SAFE and type registration is THREAD-SAFE.
    inline typeInfoBase* RegisterType( const char *typeName, typeInterface *typeInterface, typeInfoBase *inheritsFrom = nullptr )
    {
        struct typeInfoGeneral : public typeInfoBase
        {
            void Cleanup( void ) override
            {
                // Terminate ourselves.
                eir::static_del_struct <typeInfoGeneral, allocatorType> ( this->typeSys, this );
            }
        };

        typeInfoGeneral *info = eir::static_new_struct <typeInfoGeneral, allocatorType> ( this );

        if ( info )
        {
            try
            {
                SetupTypeInfoBase( info, typeName, typeInterface, inheritsFrom );
            }
            catch( ... )
            {
                eir::static_del_struct <typeInfoGeneral, allocatorType> ( this, info );

                throw;
            }
        }

        return info;
    }

    // Do no manually construct or destroy this class.
    template <typename structTypeTypeInterface>
    struct commonTypeInfoStruct : public typeInfoBase
    {
        template <typename... constrArgs>
        AINLINE commonTypeInfoStruct( constrArgs&&... args ) : tInterface( std::forward <constrArgs> ( args )... )
        {
            return;
        }

        void Cleanup( void ) override
        {
            eir::static_del_struct <commonTypeInfoStruct, allocatorType> ( this->typeSys, this );
        }

        structTypeTypeInterface tInterface;
    };

    // THREAD-SAFE because memory allocation is THREAD-SAFE and type registration is THREAD-SAFE.
    template <typename structTypeTypeInterface, typename... constrArgs>
    inline commonTypeInfoStruct <structTypeTypeInterface>* RegisterCommonTypeInterface( const char *typeName, typeInfoBase *inheritsFrom = nullptr, constrArgs&&... args )
    {
        typedef commonTypeInfoStruct <structTypeTypeInterface> typeInfoStruct;

        typeInfoStruct *tInfo = eir::static_new_struct <typeInfoStruct, allocatorType> ( this, std::forward <constrArgs> ( args )... );

        if ( tInfo == nullptr )
        {
            return nullptr;
        }

        try
        {
            SetupTypeInfoBase( tInfo, typeName, &tInfo->tInterface, inheritsFrom );
        }
        catch( ... )
        {
            eir::static_del_struct <typeInfoStruct, allocatorType> ( this, tInfo );

            throw;
        }

        return tInfo;
    }

    template <typename structType>
    inline typeInfoBase* RegisterAbstractType( const char *typeName, typeInfoBase *inheritsFrom = nullptr )
    {
        struct structTypeInterface : public typeInterface
        {
            void Construct( void *mem, systemPointer_t *sysPtr, void *construct_params ) const override
            {
                throw abstraction_construction_exception();
            }

            void CopyConstruct( void *mem, const void *srcMem ) const override
            {
                throw abstraction_construction_exception();
            }

            void Destruct( void *mem ) const override
            {
                return;
            }

            size_t GetTypeSize( systemPointer_t *sysPtr, void *construct_params ) const override
            {
                return sizeof( structType );
            }

            size_t GetTypeSizeByObject( systemPointer_t *sysPtr, const void *langObj ) const override
            {
                return (size_t)0;
            }
        };

        typeInfoBase *newTypeInfo = RegisterCommonTypeInterface <structTypeInterface> ( typeName, inheritsFrom );

        if ( newTypeInfo )
        {
            // WARNING: if you allow construction of types while types register themselves THIS IS A SECURITY ISSUE.
            // WE DO NOT DO THAT.
            newTypeInfo->isAbstract = true;
        }

        return newTypeInfo;
    }

private:
    // THREAD-SAFE Helpers for copy construction.
    template <typename structType>
    static AINLINE typename std::enable_if <std::is_copy_constructible <structType>::value, void>::type try_copy_construct( void *dstMem, const structType& right )
    {
        new (dstMem) structType( right );
    }

    template <typename structType>
    static AINLINE typename std::enable_if <!std::is_copy_constructible <structType>::value, void>::type try_copy_construct( void *dstMem, const structType& right )
    {
        throw undefined_method_exception();
    }

public:
    // THREAD-SAFE, because memory allocation is THREAD-SAFE and type registration is THREAD-SAFE.
    template <typename structType>
    inline typeInfoBase* RegisterStructType( const char *typeName, typeInfoBase *inheritsFrom = nullptr, size_t structSize = sizeof( structType ) )
    {
        struct structTypeInterface : public typeInterface
        {
            AINLINE structTypeInterface( size_t structSize )
            {
                this->structSize = structSize;
            }

            void Construct( void *mem, systemPointer_t *sysPtr, void *construct_params ) const override
            {
                new (mem) structType( sysPtr, construct_params );
            }

            void CopyConstruct( void *mem, const void *srcMem ) const override
            {
                try_copy_construct( mem, *(const structType*)srcMem );
            }

            void Destruct( void *mem ) const override
            {
                ((structType*)mem)->~structType();
            }

            size_t GetTypeSize( systemPointer_t *sysPtr, void *construct_params ) const override
            {
                return this->structSize;
            }

            size_t GetTypeSizeByObject( systemPointer_t *sysPtr, const void *langObj ) const override
            {
                return this->structSize;
            }

            size_t structSize;
        };

        return RegisterCommonTypeInterface <structTypeInterface> ( typeName, inheritsFrom, structSize );
    }

    // THREAD-SAFEty cannot be guarranteed. Use with caution.
    template <typename classType, typename staticRegistry>
    inline pluginOffset_t StaticPluginRegistryRegisterTypeConstruction( staticRegistry& registry, typeInfoBase *typeInfo, systemPointer_t *sysPtr, void *construction_params = nullptr )
    {
        struct structPluginInterface : staticRegistry::pluginInterface
        {
            bool OnPluginConstruct( typename staticRegistry::hostType_t *obj, typename staticRegistry::pluginOffset_t pluginOffset, typename staticRegistry::pluginDescriptor pluginId ) override
            {
                void *structMem = pluginId.template RESOLVE_STRUCT <void> ( obj, pluginOffset );

                if ( structMem == nullptr )
                    return false;

                // Construct the type.
                GenericRTTI *rtObj = typeSys->ConstructPlacement( obj, structMem, typeInfo, construction_params );

                // Hack: tell it about the struct.
                if ( rtObj != nullptr )
                {
                    void *langObj = DynamicTypeSystem::GetObjectFromTypeStruct( rtObj );

                    try
                    {
                        ((classType*)langObj)->Initialize( obj );
                    }
                    catch( ... )
                    {
                        typeSys->DestroyPlacement( obj, rtObj );

                        throw;
                    }
                }

                return ( rtObj != nullptr );
            }

            void OnPluginDestruct( typename staticRegistry::hostType_t *obj, typename staticRegistry::pluginOffset_t pluginOffset, typename staticRegistry::pluginDescriptor pluginId ) override
            {
                GenericRTTI *rtObj = pluginId.template RESOLVE_STRUCT <GenericRTTI> ( obj, pluginOffset );

                // Hack: deinitialize the class.
                {
                    void *langObj = DynamicTypeSystem::GetObjectFromTypeStruct( rtObj );

                    ((classType*)langObj)->Shutdown( obj );
                }

                // Destruct the type.
                typeSys->DestroyPlacement( obj, rtObj );
            }

            bool OnPluginAssign( typename staticRegistry::hostType_t *dstObject, const typename staticRegistry::hostType_t *srcObject, typename staticRegistry::pluginOffset_t pluginOffset, typename staticRegistry::pluginDescriptor pluginId ) override
            {
                return false;
            }

            void DeleteOnUnregister( void ) override
            {
                eir::static_del_struct <structPluginInterface, allocatorType> ( this->typeSys, this );
            }

            DynamicTypeSystem *typeSys;
            typeInfoBase *typeInfo;
            void *construction_params;
        };

        typename staticRegistry::pluginOffset_t offset = 0;

        structPluginInterface *tInterface = eir::static_new_struct <structPluginInterface, allocatorType> ( this );

        if ( tInterface )
        {
            tInterface->typeSys = this;
            tInterface->typeInfo = typeInfo;
            tInterface->construction_params = construction_params;

            try
            {
                offset = registry.RegisterPlugin(
                    this->GetTypeStructSize( sysPtr, typeInfo, construction_params ),
                    typename staticRegistry::pluginDescriptor( staticRegistry::ANONYMOUS_PLUGIN_ID ),
                    tInterface
                );

                if ( !staticRegistry::IsOffsetValid( offset ) )
                {
                    eir::static_del_struct <structPluginInterface, allocatorType> ( this, tInterface );
                }
            }
            catch( ... )
            {
                eir::static_del_struct <structPluginInterface, allocatorType> ( this, tInterface );

                throw;
            }
        }

        return offset;
    }

    // THREAD-SAFETY: this object is IMMUTABLE.
    struct structTypeMetaInfo abstract
    {
        virtual ~structTypeMetaInfo( void )     {}

        virtual size_t GetTypeSize( systemPointer_t *sysPtr, void *construct_params ) const = 0;

        virtual size_t GetTypeSizeByObject( systemPointer_t *sysPtr, const void *mem ) const = 0;
    };

    // THREAD-SAFE, because memory allocation is THREAD-SAFE and type registration is THREAD-SAFE.
    template <typename structType>
    inline typeInfoBase* RegisterDynamicStructType( const char *typeName, structTypeMetaInfo *metaInfo, bool freeMetaInfo, typeInfoBase *inheritsFrom = nullptr )
    {
        struct structTypeInterface : public typeInterface
        {
            AINLINE structTypeInterface( structTypeMetaInfo *meta_info, bool freeMetaInfo )
            {
                this->meta_info = meta_info;
                this->freeMetaInfo = freeMetaInfo;
            }

            inline ~structTypeInterface( void )
            {
                if ( this->freeMetaInfo )
                {
                    if ( structTypeMetaInfo *metaInfo = this->meta_info )
                    {
                        delete metaInfo;

                        this->meta_info = nullptr;
                    }
                }
            }

            void Construct( void *mem, systemPointer_t *sysPtr, void *construct_params ) const override
            {
                new (mem) structType( sysPtr, construct_params );
            }

            void CopyConstruct( void *mem, const void *srcMem ) const override
            {
                try_copy_construct( mem, *(const structType*)srcMem );
            }

            void Destruct( void *mem ) const override
            {
                ((structType*)mem)->~structType();
            }

            size_t GetTypeSize( systemPointer_t *sysPtr, void *construct_params ) const override
            {
                return meta_info->GetTypeSize( sysPtr, construct_params );
            }

            size_t GetTypeSizeByObject( systemPointer_t *sysPtr, const void *obj ) const override
            {
                return meta_info->GetTypeSizeByObject( sysPtr, obj );
            }

            structTypeMetaInfo *meta_info;
            bool freeMetaInfo;
        };

        return RegisterCommonTypeInterface <structTypeInterface> ( typeName, inheritsFrom, metaInfo, freeMetaInfo );
    }

private:
    // THREAD-SAFETY: use from LOCKED CONTEXT only (at least READ ACCESS, locked by typeInfo)!
    static inline size_t GetTypePluginSize( typeInfoBase *typeInfo )
    {
        // In the DynamicTypeSystem environment, we do not introduce conditional registry plugin structs.
        // That would complicate things too much, but support can be added if truly required.
        // Development does not have to be hell.
        // Without conditional struct support, this operation stays O(1).
        size_t sizeOut = (size_t)typeInfo->structRegistry.GetPluginSizeByRuntime();

        // Add the plugin sizes of all inherited classes.
        if ( typeInfoBase *inheritedClass = typeInfo->inheritsFrom )
        {
            sizeOut += GetTypePluginSize( inheritedClass );
        }

        return sizeOut;
    }

    // THREAD-SAFETY: use from LOCKED CONTEXT only (at least READ ACCESS, locked by typeInfo)!
    static inline size_t GetTypeStructPluginSize( typeInfoBase *typeInfo, const GenericRTTI *object )
    {
        // This method is made for conditional structing support of DTS.
        size_t sizeOut = (size_t)typeInfo->structRegistry.GetPluginSizeByObject( object );

        if ( typeInfoBase *inheritedClass = typeInfo->inheritsFrom )
        {
            sizeOut += GetTypeStructPluginSize( inheritedClass, object );
        }

        return sizeOut;
    }

    // THREAD-SAFETY: use from LOCKED CONTEXT only (at least READ ACCESS, locked by typeInfo)!
    static inline pluginOffset_t GetTypeRegisteredPluginLocation( typeInfoBase *typeInfo, const GenericRTTI *theObject, pluginOffset_t pluginOffDesc )
    {
        return typeInfo->structRegistry.ResolvePluginStructOffsetByObject( theObject, pluginOffDesc );
    }

    // THREAD-SAFETY: use from LOCKED CONTEXT only (at least READ ACCESS, locked by typeInfo)!
    // THREAD-SAFE, because called from locked context and construction of inherited plugins is THREAD-SAFE (by recursion)
    // and plugin destruction is THREAD-SAFE.
    inline bool ConstructPlugins( systemPointer_t *sysPtr, typeInfoBase *typeInfo, GenericRTTI *rtObj ) const
    {
        bool pluginConstructSuccess = false;

        // If we have no parents, it is success by default.
        bool parentConstructionSuccess = true;

        // First construct the parents.
        typeInfoBase *inheritedClass = typeInfo->inheritsFrom;

        if ( inheritedClass != nullptr )
        {
            parentConstructionSuccess = ConstructPlugins( sysPtr, inheritedClass, rtObj );
        }

        // If the parents have constructed properly, do ourselves.
        if ( parentConstructionSuccess )
        {
            bool thisConstructionSuccess = typeInfo->structRegistry.ConstructPluginBlock( rtObj, sysPtr );

            // If anything has failed, we destroy everything we already constructed.
            if ( !thisConstructionSuccess )
            {
                if ( inheritedClass != nullptr )
                {
                    DestructPlugins( sysPtr, inheritedClass, rtObj );
                }
            }
            else
            {
                pluginConstructSuccess = true;
            }
        }

        return pluginConstructSuccess;
    }

    // THREAD-SAFETY: must be called from LOCKED CONTEXT (at least READ ACCESS)!
    // THREAD-SAFE, because it is called from locked context and assigning of plugins is THREAD-SAFE (by recursion).
    inline bool AssignPlugins( systemPointer_t *sysPtr, typeInfoBase *typeInfo, GenericRTTI *dstRtObj, const GenericRTTI *srcRtObj ) const
    {
        // Assign plugins from the ground up.
        // If anything fails assignment, we just bail.
        // Failure in assignment does not imply an incorrect object state.
        bool assignmentSuccess = false;

        // First assign the parents.
        bool parentAssignmentSuccess = true;

        typeInfoBase *inheritedType = typeInfo->inheritsFrom;

        if ( inheritedType != nullptr )
        {
            parentAssignmentSuccess = AssignPlugins( sysPtr, inheritedType, dstRtObj, srcRtObj );
        }

        // If all the parents successfully assigned, we can try assigning ourselves.
        if ( parentAssignmentSuccess )
        {
            bool thisAssignmentSuccess = typeInfo->structRegistry.AssignPluginBlock( dstRtObj, srcRtObj, sysPtr );

            // If assignment was successful, we are happy.
            // Otherwise we bail the entire thing.
            if ( thisAssignmentSuccess )
            {
                assignmentSuccess = true;
            }
        }

        return assignmentSuccess;
    }

    // THREAD-SAFETY: call from LOCKED CONTEXT only (at least READ ACCESS)!
    // THREAD-SAFE, because called from locked context and plugin destruction is THREAD-SAFE (by recursion).
    inline void DestructPlugins( systemPointer_t *sysPtr, typeInfoBase *typeInfo, GenericRTTI *rtObj ) const
    {
        try
        {
            typeInfo->structRegistry.DestroyPluginBlock( rtObj, sysPtr );
        }
        catch( ... )
        {
            rtti_assert( 0 );
        }

        if ( typeInfoBase *inheritedClass = typeInfo->inheritsFrom )
        {
            DestructPlugins( sysPtr, inheritedClass, rtObj );
        }
    }

public:
    // THREAD-SAFE, because many operations are immutable and GetTypePluginSize is called from LOCKED READ CONTEXT.
    inline size_t GetTypeStructSize( systemPointer_t *sysPtr, typeInfoBase *typeInfo, void *construct_params ) const
    {
        typeInterface *tInterface = typeInfo->tInterface;

        // Attempt to get the memory the language object will take.
        size_t objMemSize = tInterface->GetTypeSize( sysPtr, construct_params );

        if ( objMemSize != 0 )
        {
            // Adjust that objMemSize so we can store meta information + plugins.
            objMemSize += sizeof( GenericRTTI );

            scoped_rwlock_read typeLock( this->lockProvider, typeInfo->typeLock );

            // Calculate the memory that is required by all plugin structs.
            objMemSize += GetTypePluginSize( typeInfo );
        }

        return objMemSize;
    }

    // THREAD-SAFE, because many operations are immutable and GetTypePluginSize is called from LOCKED READ CONTEXT.
    inline size_t GetTypeStructSize( systemPointer_t *sysPtr, const GenericRTTI *rtObj ) const
    {
        typeInfoBase *typeInfo = GetTypeInfoFromTypeStruct( rtObj );
        typeInterface *tInterface = typeInfo->tInterface;

        // Get the pointer to the object.
        const void *langObj = GetConstObjectFromTypeStruct( rtObj );

        // Get the memory that is taken by the language object.
        size_t objMemSize = tInterface->GetTypeSizeByObject( sysPtr, langObj );

        if ( objMemSize != 0 )
        {
            // Take the meta data into account.
            objMemSize += sizeof( GenericRTTI );

            scoped_rwlock_read typeLock( this->lockProvider, typeInfo->typeLock );

            // Add the memory taken by the plugins.
            objMemSize += GetTypeStructPluginSize( typeInfo, rtObj );
        }

        return objMemSize;
    }

    // THREAD-SAFE, because it establishes a write context.
    inline void ReferenceTypeInfo( typeInfoBase *typeInfo )
    {
        scoped_rwlock_write lock( this->lockProvider, typeInfo->typeLock );

        // This turns the typeInfo IMMUTABLE.
        typeInfo->refCount++;

        // For every type we inherit, we reference it as well.
        if ( typeInfoBase *inheritedClass = typeInfo->inheritsFrom )
        {
            ReferenceTypeInfo( inheritedClass );
        }
    }

    // THREAD-SAFE, because it establishes a write context.
    inline void DereferenceTypeInfo( typeInfoBase *typeInfo )
    {
        scoped_rwlock_write lock( this->lockProvider, typeInfo->typeLock );

        // For every type we inherit, we dereference it as well.
        if ( typeInfoBase *inheritedClass = typeInfo->inheritsFrom )
        {
            DereferenceTypeInfo( inheritedClass );
        }

        // This could turn the typeInfo NOT IMMUTABLE.
        typeInfo->refCount--;
    }

    // THREAD-SAFE, because the typeInterface is THREAD-SAFE and plugin construction is THREAD-SAFE.
    inline GenericRTTI* ConstructPlacement( systemPointer_t *sysPtr, void *objMem, typeInfoBase *typeInfo, void *construct_params )
    {
        GenericRTTI *objOut = nullptr;
        {
            // Reference the type info.
            ReferenceTypeInfo( typeInfo );

            // NOTE: TYPE INFOs do NOT CHANGE while they are referenced.
            // That is why we can loop through the plugin containers without referencing
            // the type infos!

            // Get the specialization interface.
            typeInterface *tInterface = typeInfo->tInterface;

            // Get a pointer to GenericRTTI and the object memory.
            GenericRTTI *objTypeMeta = (GenericRTTI*)objMem;

            // Initialize the RTTI struct.
            objTypeMeta->type_meta = typeInfo;
#ifdef _DEBUG
            objTypeMeta->typesys_ptr = this;
#endif //_DEBUG

            // Initialize the language object.
            void *objStruct = objTypeMeta + 1;

            try
            {
                // Attempt to construct the language part.
                tInterface->Construct( objStruct, sysPtr, construct_params );
            }
            catch( ... )
            {
                // We failed to construct the object struct, so it is invalid.
                objStruct = nullptr;
            }

            if ( objStruct )
            {
                // Only proceed if we have successfully constructed the object struct.
                // Now construct the plugins.
                bool pluginConstructSuccess = ConstructPlugins( sysPtr, typeInfo, objTypeMeta );

                if ( pluginConstructSuccess )
                {
                    // We are finished! Return the meta info.
                    objOut = objTypeMeta;
                }
                else
                {
                    // We failed, so destruct the class again.
                    tInterface->Destruct( objStruct );
                }
            }

            if ( objOut == nullptr )
            {
                // Since we did not return a proper object, dereference again.
                DereferenceTypeInfo( typeInfo );
            }
        }
        return objOut;
    }

    // THREAD-SAFE, because memory allocation is THREAD-SAFE, GetTypeStructSize is THREAD-SAFE and ConstructPlacement is THREAD-SAFE.
    inline GenericRTTI* Construct( systemPointer_t *sysPtr, typeInfoBase *typeInfo, void *construct_params )
    {
        GenericRTTI *objOut = nullptr;
        {
            // We must reference the type info to prevent the exploit where the type struct can change
            // during construction.
            ReferenceTypeInfo( typeInfo );

            try
            {
                size_t objMemSize = GetTypeStructSize( sysPtr, typeInfo, construct_params );

                if ( objMemSize != 0 )
                {
                    void *objMem = allocatorType::Allocate( this, objMemSize, STANDARD_OBJECT_ALIGNMENT );

                    if ( objMem )
                    {
                        try
                        {
                            // Attempt to construct the object on the memory.
                            objOut = ConstructPlacement( sysPtr, objMem, typeInfo, construct_params );
                        }
                        catch( ... )
                        {
                            // Just to be on the safe side.
                            allocatorType::Free( this, objMem );

                            throw;
                        }

                        if ( !objOut )
                        {
                            // Deallocate the memory again, as we seem to have failed.
                            allocatorType::Free( this, objMem );
                        }
                    }
                }
            }
            catch( ... )
            {
                // Just to be on the safe side.
                DereferenceTypeInfo( typeInfo );

                throw;
            }

            // We can dereference the type info again.
            DereferenceTypeInfo( typeInfo );
        }
        return objOut;
    }

    // THREAD-SAFE, because many operations are IMMUTABLE, the type interface is THREAD-SAFE, plugin construction
    // is called from LOCKED READ CONTEXT and plugin assignment is called from LOCKED READ CONTEXT.
    inline GenericRTTI* ClonePlacement( systemPointer_t *sysPtr, void *objMem, const GenericRTTI *toBeCloned )
    {
        GenericRTTI *objOut = nullptr;
        {
            // Grab the type of toBeCloned.
            typeInfoBase *typeInfo = GetTypeInfoFromTypeStruct( toBeCloned );

            // Reference the type info.
            ReferenceTypeInfo( typeInfo );

            try
            {
                // Get the specialization interface.
                const typeInterface *tInterface = typeInfo->tInterface;

                // Get a pointer to GenericRTTI and the object memory.
                GenericRTTI *objTypeMeta = (GenericRTTI*)objMem;

                // Initialize the RTTI struct.
                objTypeMeta->type_meta = typeInfo;
#ifdef _DEBUG
                objTypeMeta->typesys_ptr = this;
#endif //_DEBUG

                // Initialize the language object.
                void *objStruct = objTypeMeta + 1;

                // Get the struct which we create from.
                const void *srcObjStruct = toBeCloned + 1;

                try
                {
                    // Attempt to copy construct the language part.
                    tInterface->CopyConstruct( objStruct, srcObjStruct );
                }
                catch( ... )
                {
                    // We failed to construct the object struct, so it is invalid.
                    objStruct = nullptr;
                }

                if ( objStruct )
                {
                    // Only proceed if we have successfully constructed the object struct.
                    // First we have to construct the plugins.
                    bool pluginSuccess = false;

                    bool pluginConstructSuccess = ConstructPlugins( sysPtr, typeInfo, objTypeMeta );

                    if ( pluginConstructSuccess )
                    {
                        // Now assign the plugins from the source object.
                        bool pluginAssignSuccess = AssignPlugins( sysPtr, typeInfo, objTypeMeta, toBeCloned );

                        if ( pluginAssignSuccess )
                        {
                            // We are finished! Return the meta info.
                            pluginSuccess = true;
                        }
                    }

                    if ( pluginSuccess )
                    {
                        objOut = objTypeMeta;
                    }
                    else
                    {
                        // We failed, so destruct the class again.
                        tInterface->Destruct( objStruct );
                    }
                }
            }
            catch( ... )
            {
                // Just to be on the safe side.
                DereferenceTypeInfo( typeInfo );

                throw;
            }

            if ( objOut == nullptr )
            {
                // Since we did not return a proper object, dereference again.
                DereferenceTypeInfo( typeInfo );
            }
        }
        return objOut;
    }

    // THREAD-SAFE, because GetTypeStructSize is THREAD-SAFE, memory allocation is THREAD-SAFE
    // and ClonePlacement is THREAD_SAFE.
    inline GenericRTTI* Clone( systemPointer_t *sysPtr, const GenericRTTI *toBeCloned )
    {
        GenericRTTI *objOut = nullptr;
        {
            // Get the size toBeCloned currently takes.
            // This is an immutable property, because memory cannot magically expand.
            size_t objMemSize = GetTypeStructSize( sysPtr, toBeCloned );

            if ( objMemSize != 0 )
            {
                void *objMem = allocatorType::Allocate( this, objMemSize, STANDARD_OBJECT_ALIGNMENT );

                if ( objMem )
                {
                    // Attempt to clone the object on the memory.
                    objOut = ClonePlacement( sysPtr, objMem, toBeCloned );
                }

                if ( !objOut )
                {
                    // Deallocate the memory again, as we seem to have failed.
                    allocatorType::Free( this, objMem );
                }
            }
        }
        return objOut;
    }

    // THREAD-SAFE, because single atomic operation.
    inline void SetTypeInfoExclusive( typeInfoBase *typeInfo, bool isExclusive )
    {
        typeInfo->isExclusive = isExclusive;
    }

    // THREAD-SAFE, because single atomic operation.
    inline bool IsTypeInfoExclusive( typeInfoBase *typeInfo )
    {
        return typeInfo->isExclusive;
    }

    // THREAD-SAFE, because single atomic operation.
    inline bool IsTypeInfoAbstract( typeInfoBase *typeInfo )
    {
        return typeInfo->isAbstract;
    }

    // THREAD-SAFE, because we lock very hard to ensure consistent state.
    inline void SetTypeInfoInheritingClass( typeInfoBase *subClass, typeInfoBase *inheritedClass, bool requiresSystemLock = true )
    {
        bool subClassImmutability = subClass->IsImmutable();

        rtti_assert( subClassImmutability == false );

        if ( subClassImmutability == false )
        {
            // We have to lock here, because this has to happen atomically on the whole type system.
            // Because inside of the whole type system, we assume there is not another type with the same resolution.
            scoped_rwlock_write sysLock( this->lockProvider, ( requiresSystemLock ? this->mainLock : nullptr ) );

            // Make sure we can even do that.
            // Verify that no other type with that name exists which inherits from said class.
            if ( inheritedClass != nullptr )
            {
                typeInfoBase *alreadyExisting = FindTypeInfoNolock( subClass->name, inheritedClass );

                if ( alreadyExisting && alreadyExisting != subClass )
                {
                    throw type_name_conflict_exception();
                }
            }

            // Alright, now we have confirmed the operation.
            // We want to change the state of the type, so we must write lock to ensure a consistent state.
            scoped_rwlock_write typeLock( this->lockProvider, subClass->typeLock );

            typeInfoBase *prevInherit = subClass->inheritsFrom;

            if ( prevInherit != inheritedClass )
            {
                scoped_rwlock_write inheritedLock( this->lockProvider, ( prevInherit ? prevInherit->typeLock : nullptr ) );
                scoped_rwlock_write newInheritLock( this->lockProvider, ( inheritedClass ? inheritedClass->typeLock : nullptr ) );

                if ( inheritedClass != nullptr )
                {
                    // Make sure that we NEVER do circular inheritance!
                    rtti_assert( IsTypeInheritingFromNolock( subClass, inheritedClass ) == false );
                }

                if ( prevInherit )
                {
                    prevInherit->inheritanceCount--;
                }

                subClass->inheritsFrom = inheritedClass;

                if ( inheritedClass )
                {
                    inheritedClass->inheritanceCount++;
                }
            }
        }
    }

private:
    // THREAD-SAFETY: has to be called from LOCKED READ CONTEXT (by subClass lock)!
    // THIS IS NOT A THREAD-SAFE ALGORITHM; USE WITH CAUTION.
    inline bool IsTypeInheritingFromNolock( typeInfoBase *baseClass, typeInfoBase *subClass ) const
    {
        if ( IsSameType( baseClass, subClass ) )
        {
            return true;
        }

        typeInfoBase *inheritedClass = subClass->inheritsFrom;

        if ( inheritedClass )
        {
            return IsTypeInheritingFromNolock( baseClass, inheritedClass );
        }

        return false;
    }

public:
    // THREAD-SAFE, because type equality is IMMUTABLE property, inherited class is
    // being verified under LOCKED READ CONTEXT and type inheritance check is THREAD-SAFE
    // (by recursion).
    inline bool IsTypeInheritingFrom( typeInfoBase *baseClass, typeInfoBase *subClass ) const
    {
        // We do not have to lock on this, because equality is an IMMUTABLE property of types.
        if ( IsSameType( baseClass, subClass ) )
        {
            return true;
        }

        scoped_rwlock_read subClassLock( this->lockProvider, subClass->typeLock );

        typeInfoBase *inheritedClass = subClass->inheritsFrom;

        if ( inheritedClass )
        {
            return IsTypeInheritingFrom( baseClass, inheritedClass );
        }

        return false;
    }

    // THREAD-SAFE, because single atomic operation.
    inline bool IsSameType( typeInfoBase *firstType, typeInfoBase *secondType ) const
    {
        return ( firstType == secondType );
    }

    // THREAD-SAFE, because local atomic operation.
    static inline void* GetObjectFromTypeStruct( GenericRTTI *rtObj )
    {
        return (void*)( rtObj + 1 );
    }

    // THREAD-SAFE, because local atomic operation.
    static inline const void* GetConstObjectFromTypeStruct( const GenericRTTI *rtObj )
    {
        return (void*)( rtObj + 1 );
    }

    // THREAD-SAFE, because local atomic operation.
    static inline GenericRTTI* GetTypeStructFromObject( void *langObj )
    {
        return ( (GenericRTTI*)langObj - 1 );
    }

    // THREAD-SAFE, because local atomic operation.
    static inline const GenericRTTI* GetTypeStructFromConstObject( const void *langObj )
    {
        return ( (const GenericRTTI*)langObj - 1 );
    }

protected:
    inline void DebugRTTIStruct( const GenericRTTI *typeInfo ) const
    {
#ifdef _DEBUG
        // If this assertion fails, the runtime may have mixed up times from different contexts (i.e. different configurations in Lua)
        // The problem is certainly found in the application itself.
        rtti_assert( typeInfo->typesys_ptr == this );
#endif //_DEBUG
    }

public:
    // THREAD-SAFE, because local atomic operation.
    inline GenericRTTI* GetTypeStructFromAbstractObject( void *langObj )
    {
        GenericRTTI *typeInfo = ( (GenericRTTI*)langObj - 1 );

        // Make sure it is valid.
        DebugRTTIStruct( typeInfo );

        return typeInfo;
    }

    // THREAD-SAFE, because local atomic operation.
    inline const GenericRTTI* GetTypeStructFromConstAbstractObject( const void *langObj ) const
    {
        const GenericRTTI *typeInfo = ( (const GenericRTTI*)langObj - 1 );

        // Make sure it is valid.
        DebugRTTIStruct( typeInfo );

        return typeInfo;
    }

    // THREAD-SAFE, because DestructPlugins is called from LOCKED READ CONTEXT and type interface
    // is THREAD-SAFE.
    inline void DestroyPlacement( systemPointer_t *sysPtr, GenericRTTI *typeStruct )
    {
        typeInfoBase *typeInfo = GetTypeInfoFromTypeStruct( typeStruct );
        typeInterface *tInterface = typeInfo->tInterface;

        // Destroy all object plugins.
        DestructPlugins( sysPtr, typeInfo, typeStruct );

        // Pointer to the language object.
        void *langObj = (void*)( typeStruct + 1 );

        // Destroy the actual object.
        try
        {
            tInterface->Destruct( langObj );
        }
        catch( ... )
        {
            // We cannot handle this, since it must not happen in the first place.
            rtti_assert( 0 );
        }

        // Dereference the type info since we do not require it anymore.
        DereferenceTypeInfo( typeInfo );
    }

    // THREAD-SAFE, because typeStruct memory size if an IMMUTABLE property,
    // DestroyPlacement is THREAD-SAFE and memory allocation is THREAD-SAFE.
    inline void Destroy( systemPointer_t *sysPtr, GenericRTTI *typeStruct )
    {
        // Delete the object from the memory.
        DestroyPlacement( sysPtr, typeStruct );

        // Free the memory.
        void *rttiMem = (void*)typeStruct;

        allocatorType::Free( this, rttiMem );
    }

    // Calling this method is only permitted on types that YOU KNOW
    // ARE NOT USED ANYMORE. In a multi-threaded environment this is
    // VERY DANGEROUS. The runtime itself must ensure that typeInfo
    // cannot be addressed by any logic!
    // Under the above assumption, this function is THREAD-SAFE.
    // THREAD-SAFE, because typeInfo is not used anymore and a global
    // system lock is used when handling the type environment.
    inline void DeleteType( typeInfoBase *typeInfo )
    {
        // Make sure we do not inherit from anything anymore.
        if ( typeInfo->inheritsFrom != nullptr )
        {
            scoped_rwlock_write sysLock( this->lockProvider, this->mainLock );

            typeInfoBase *inheritsFrom = typeInfo->inheritsFrom;

            if ( inheritsFrom )
            {
                scoped_rwlock_write inheritedLock( this->lockProvider, inheritsFrom->typeLock );

                typeInfo->inheritsFrom = nullptr;

                inheritsFrom->inheritanceCount--;
            }
        }

        // Make sure all classes that inherit from us do not do that anymore.
        {
            scoped_rwlock_write typeEnvironmentConsistencyLock( this->lockProvider, this->mainLock );

            LIST_FOREACH_BEGIN( typeInfoBase, this->registeredTypes.root, node )

                if ( item->inheritsFrom == typeInfo )
                {
                    SetTypeInfoInheritingClass( item, nullptr, false );
                }

            LIST_FOREACH_END
        }

        if ( typeInfo->typeLock )
        {
            // The lock provider is assumed to be THREAD-SAFE itself.
            this->lockProvider.CloseLock( typeInfo->typeLock );
        }

        // Remove this type from this manager.
        {
            scoped_rwlock_write sysLock( this->lockProvider, this->mainLock );

            LIST_REMOVE( typeInfo->node );
        }

        typeInfo->Cleanup();
    }

    // THREAD-SAFE, because it uses GLOBAL SYSTEM READ LOCK when iterating through the type nodes.
    struct type_iterator
    {
        DynamicTypeSystem& typeSys;

        // When iterating through types, we must hold a lock.
        scoped_rwlock_read typeConsistencyLock;

        const RwList <typeInfoBase>& listRoot;
        RwListEntry <typeInfoBase> *curNode;

        inline type_iterator( DynamicTypeSystem& typeSys )
            : typeSys( typeSys ),
              typeConsistencyLock( typeSys.lockProvider, typeSys.mainLock ),    // WE MUST GET THE LOCK BEFORE WE ACQUIRE THE LIST ROOT, for nicety :3
              listRoot( typeSys.registeredTypes )
        {
            this->curNode = listRoot.root.next;
        }

        inline type_iterator( const type_iterator& right )
            : typeSys( right.typeSys ),
              typeConsistencyLock( typeSys.lockProvider, typeSys.mainLock ),
              listRoot( right.listRoot )
        {
            this->curNode = right.curNode;
        }

        inline type_iterator( type_iterator&& right )
            : typeSys( right.typeSys ),
              typeConsistencyLock( typeSys.lockProvider, typeSys.mainLock ),
              listRoot( right.listRoot )
        {
            this->curNode = right.curNode;
        }

        inline bool IsEnd( void ) const
        {
            return ( &listRoot.root == curNode );
        }

        inline typeInfoBase* Resolve( void ) const
        {
            return LIST_GETITEM( typeInfoBase, curNode, node );
        }

        inline void Increment( void )
        {
            this->curNode = this->curNode->next;
        }
    };

    // THREAD-SAFE, because it returns THREAD-SAFE type_iterator object.
    inline type_iterator GetTypeIterator( void ) const
    {
        // Just hack around, meow.
        return type_iterator( *(DynamicTypeSystem*)this );
    }

private:
    struct dtsAllocLink
    {
        inline dtsAllocLink( DynamicTypeSystem *refMem )
        {
            this->refMem = refMem;
        }

        inline dtsAllocLink( const dtsAllocLink& right ) = default;

        inline void* Allocate( void *_, size_t memSize, size_t alignment )
        {
            return allocatorType::Allocate( this>refMem, memSize, alignment );
        }

        inline bool Resize( void *_, void *memPtr, size_t reqSize )
        {
            return allocatorType::Resize( this->refMem, memPtr, reqSize );
        }

        inline void Free( void *_, void *memPtr )
        {
            allocatorType::Free( this->refMem, memPtr );
        }

        DynamicTypeSystem *refMem;

        struct is_object {};
    };

public:
    typedef eir::String <char, dtsAllocLink> dtsString;

    // THREAD-SAFE, because it only consists of THREAD-SAFE local operations.
    struct type_resolution_iterator
    {
        DynamicTypeSystem *typeSys;
        const char *typePath;
        const char *type_iter_ptr;
        size_t tokenLen;

        inline type_resolution_iterator( DynamicTypeSystem *typeSys, const char *typePath )
        {
            this->typeSys = typeSys;
            this->typePath = typePath;
            this->type_iter_ptr = typePath;
            this->tokenLen = 0;

            this->Increment();
        }

        inline dtsString Resolve( void ) const
        {
            const char *returnedPathToken = this->typePath;

            size_t nameLength = this->tokenLen;

            return dtsString( returnedPathToken, nameLength, this->typeSys );
        }

        inline void Increment( void )
        {
            this->typePath = this->type_iter_ptr;

            size_t currentTokenLen = 0;

            while ( true )
            {
                const char *curPtr = this->type_iter_ptr;

                char c = *curPtr;

                if ( c == 0 )
                {
                    currentTokenLen = ( curPtr - this->typePath );

                    break;
                }

                if ( strncmp( curPtr, "::", 2 ) == 0 )
                {
                    currentTokenLen = ( curPtr - this->typePath );

                    this->type_iter_ptr = curPtr + 2;

                    break;
                }

                this->type_iter_ptr++;
            }

            // OK.
            this->tokenLen = currentTokenLen;
        }

        inline bool IsEnd( void ) const
        {
            return ( this->typePath == this->type_iter_ptr );
        }
    };

private:
    // THREAD-SAFETY: call from GLOBAL LOCKED READ CONTEXT only!
    // THREAD-SAFE, because called from global LOCKED READ CONTEXT.
    inline typeInfoBase* FindTypeInfoNolock( const char *typeName, typeInfoBase *baseType ) const
    {
        LIST_FOREACH_BEGIN( typeInfoBase, this->registeredTypes.root, node )

            bool isInterestingType = true;

            if ( baseType != item->inheritsFrom )
            {
                isInterestingType = false;
            }

            if ( isInterestingType && strcmp( item->name, typeName ) == 0 )
            {
                return item;
            }

        LIST_FOREACH_END

        return nullptr;
    }

public:
    // THREAD-SAFE, because it calls FindTypeInfoNolock using GLOBAL LOCKED READ CONTEXT.
    inline typeInfoBase* FindTypeInfo( const char *typeName, typeInfoBase *baseType ) const
    {
        scoped_rwlock_read lock( this->lockProvider, this->mainLock );

        return FindTypeInfoNolock( typeName, baseType );
    }

    // Type resolution based on type descriptors.
    // THREAD-SAFE, because it uses THREAD-SAFE type_resolution_iterator object and the
    // FindTypeInfo function is THREAD-SAFE.
    inline typeInfoBase* ResolveTypeInfo( const char *typePath, typeInfoBase *baseTypeInfo = nullptr )
    {
        typeInfoBase *returnedTypeInfo = nullptr;
        {
            typeInfoBase *currentType = baseTypeInfo;

            // Find a base type.
            type_resolution_iterator type_path_iter( this, typePath );

            while ( type_path_iter.IsEnd() == false )
            {
                dtsString curToken = type_path_iter.Resolve();

                currentType = FindTypeInfo( curToken.c_str(), currentType );

                if ( currentType == nullptr )
                {
                    break;
                }

                type_path_iter.Increment();
            }

            returnedTypeInfo = currentType;
        }
        return returnedTypeInfo;
    }
};

// Redirect from structRegistry to typeInfoBase.
DTS_TEMPLARGS
IMPL_HEAP_REDIR_METH_ALLOCATE_RETURN DynamicTypeSystem DTS_TEMPLUSE::structRegRedirAlloc::Allocate IMPL_HEAP_REDIR_METH_ALLOCATE_ARGS
{
    typeInfoBase *hostStruct = LIST_GETITEM( typeInfoBase, refMem, structRegistry );
    return allocatorType::Allocate( hostStruct->typeSys, memSize, alignment );
}
DTS_TEMPLARGS
IMPL_HEAP_REDIR_METH_RESIZE_RETURN DynamicTypeSystem DTS_TEMPLUSE::structRegRedirAlloc::Resize IMPL_HEAP_REDIR_METH_RESIZE_ARGS
{
    typeInfoBase *hostStruct = LIST_GETITEM( typeInfoBase, refMem, structRegistry );
    return allocatorType::Resize( hostStruct->typeSys, objMem, reqNewSize );
}
DTS_TEMPLARGS
IMPL_HEAP_REDIR_METH_FREE_RETURN DynamicTypeSystem DTS_TEMPLUSE::structRegRedirAlloc::Free IMPL_HEAP_REDIR_METH_FREE_ARGS
{
    typeInfoBase *hostStruct = LIST_GETITEM( typeInfoBase, refMem, structRegistry );
    allocatorType::Free( hostStruct->typeSys, memPtr );
}

#endif //_DYN_TYPE_ABSTRACTION_SYSTEM_
