#include "group_shared.hpp"
#include <cassert>

// Does not work for windows yet
#ifndef _MSC_VER

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

using namespace tightdb;

// Pre-declare local functions
char* concat_strings(const char* str1, const char* str2);

struct tightdb::ReadCount {
    uint32_t version;
    uint32_t count;
};

struct tightdb::SharedInfo {
    pthread_mutex_t readmutex;
    pthread_mutex_t writemutex;
    uint64_t filesize;
    uint32_t infosize;
    
    uint64_t current_top;
    uint32_t current_version;
    
    uint32_t capacity; // -1 so it can also be used as mask
    uint32_t put_pos;
    uint32_t get_pos;
    ReadCount readers[32]; // has to be power of two
};

SharedGroup::SharedGroup(const char* filename) : m_group(filename, false), m_info(NULL), m_isValid(false), m_version(-1)
{
    if (!m_group.is_valid()) return;
    
    // Open shared coordination buffer
    const char* const path = concat_strings(filename, ".lock");
    const int fd = open(path, O_RDWR|O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    free((void*)path); 
    if (fd < 0) return;
    
    // Get size
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        close(fd);
        return;
    }
    size_t len = statbuf.st_size;
    
    // Handle empty files (first user)
    bool needInit = false;
    if (len == 0) {
        // Create new file
        len = sizeof(SharedInfo);
        const int r = ftruncate(fd, len);
        if (r != 0) {
            close(fd);
            return;
        }
        needInit = true;
    }
    
    // Map to memory
    void* const p = mmap(0, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // no loner need file discriptor
    if (p == (void*)-1) return;

    m_info = (SharedInfo*)p;
    
    if (needInit) {
        pthread_mutex_lock(&m_info->readmutex);
        {
            const SlabAlloc& alloc = m_group.get_allocator();
            
            m_info->filesize = alloc.GetFileLen();
            m_info->infosize = (uint32_t)len;
            m_info->current_top = alloc.GetTopRef();;
            m_info->current_version = 0;
            m_info->capacity = 32-1; 
            m_info->put_pos = 0;
            m_info->get_pos = 0;
        }
        pthread_mutex_unlock(&m_info->readmutex);
    }
    
    m_isValid = true;
}

SharedGroup::~SharedGroup()
{
    if (m_info) {
        munmap((void*)m_info, m_info->infosize); 
    }
}

const Group& SharedGroup::start_read()
{
    size_t new_topref = 0;
    size_t new_filesize = 0;
    
    pthread_mutex_lock(&m_info->readmutex);
    {
        // Get the current top ref
        new_topref   = m_info->current_top;
        new_filesize = m_info->filesize;
        m_version    = m_info->current_version;
        
        // Update reader list
        if (ringbuf_is_empty()) {
            const ReadCount r2 = {m_info->current_version, 1};
            ringbuf_put(r2);
        }
        else {
            ReadCount& r = ringbuf_get_last();
            if (r.version == m_info->current_version)
                ++(r.count);
            else {
                const ReadCount r2 = {m_info->current_version, 1};
                ringbuf_put(r2);
            }
        }
    }
    pthread_mutex_unlock(&m_info->readmutex);
    
    // Make sure the group is up-to-date
    m_group.update_from_shared(new_topref, new_filesize);
    
    return m_group;
}

void SharedGroup::end_read()
{
    assert(m_version != (uint32_t)-1);
    
    pthread_mutex_lock(&m_info->readmutex);
    {
        // Find entry for current version
        const size_t ndx = ringbuf_find(m_version);
        assert(ndx != (size_t)-1);
        ReadCount& r = ringbuf_get(ndx);
        
        // Decrement count and remove as many entries as possible
        if (r.count == 1 && ringbuf_is_first(ndx)) {
            ringbuf_remove_first();
            while (!ringbuf_is_empty() && ringbuf_get_first().count == 0) {
                ringbuf_remove_first();
            }
        }
        else {
            assert(r.count > 0);
            --r.count;
        }
    }
    pthread_mutex_unlock(&m_info->readmutex);
    
    m_version = (uint32_t)-1;
}

Group& SharedGroup::start_write()
{
    // Get write lock
    // Note that this will not get released until we call
    // end_write().
    pthread_mutex_lock(&m_info->writemutex);
    
    // Get the current top ref
    const size_t new_topref   = m_info->current_top;
    const size_t new_filesize = m_info->filesize;
    
    // Make sure the group is up-to-date
    // zero ref means that the file has just been created
    if (new_topref != 0) {
        m_group.update_from_shared(new_topref, new_filesize);
    }
    
    return m_group;
}

void SharedGroup::end_write()
{
    m_group.commit();
    
    // Get the current top ref
    const SlabAlloc& alloc = m_group.get_allocator();
    const size_t new_topref   = alloc.GetTopRef();
    const size_t new_filesize = alloc.GetFileLen();
    
    // Update reader info
    pthread_mutex_lock(&m_info->readmutex);
    {
        m_info->current_top = new_topref;
        m_info->filesize    = new_filesize;
        ++m_info->current_version;
    }
    pthread_mutex_unlock(&m_info->readmutex);
    
    // Release write lock
    pthread_mutex_unlock(&m_info->writemutex);
}

bool SharedGroup::ringbuf_is_empty() const
{
    return (ringbuf_size() == 0);
}

size_t SharedGroup::ringbuf_size() const
{
    return ((m_info->put_pos - m_info->get_pos) & m_info->capacity);
}

size_t SharedGroup::ringbuf_capacity() const
{
    return m_info->capacity+1;
}

bool SharedGroup::ringbuf_is_first(size_t ndx) const {
    return (ndx == m_info->get_pos);
}

ReadCount& SharedGroup::ringbuf_get(size_t ndx)
{
    return m_info->readers[ndx];
}

ReadCount& SharedGroup::ringbuf_get_first()
{
    return m_info->readers[m_info->get_pos];
}

ReadCount& SharedGroup::ringbuf_get_last()
{
    const uint32_t lastPos = (m_info->put_pos - 1) & m_info->capacity;
    return m_info->readers[lastPos];
}

void SharedGroup::ringbuf_remove_first() {
    m_info->get_pos = (m_info->get_pos + 1) & m_info->capacity;
}

void SharedGroup::ringbuf_put(const ReadCount& v)
{
    const bool isFull = (ringbuf_size() == (m_info->capacity+1));
    
    if(isFull) {
        //TODO: expand buffer
        assert(false);
    }
    
    m_info->readers[m_info->put_pos] = v;
    m_info->put_pos = (m_info->put_pos + 1) & m_info->capacity;
}

size_t SharedGroup::ringbuf_find(uint32_t version) const
{
    uint32_t pos = m_info->get_pos;
    while (pos != m_info->put_pos) {
        const ReadCount& r = m_info->readers[pos];
        if (r.version == version)
            return pos;
        
        pos = (pos + 1) & m_info->capacity;
    }
    
    return (size_t)-1;
}

#ifdef _DEBUG

void SharedGroup::test_ringbuf()
{
    assert(ringbuf_is_empty());
    
    const ReadCount rc = {1, 1};
    ringbuf_put(rc);
    assert(ringbuf_size() == 1);
    
    ringbuf_remove_first();
    assert(ringbuf_is_empty());
    
    // Fill buffer
    const size_t capacity = ringbuf_capacity();
    for (size_t i = 0; i < capacity; ++i) {
        const ReadCount r = {1, (uint32_t)i};
        ringbuf_put(r);
        assert(ringbuf_get_last().count == i);
    }
    for (size_t i = 0; i < 32; ++i) {
        const ReadCount& r = ringbuf_get_first();
        assert(r.count == i);

        ringbuf_remove_first();
    }
    assert(ringbuf_is_empty());
    
}

#endif //_DEBUG

#endif //_MSV_VER

// Support methods
char* concat_strings(const char* str1, const char* str2) {
    const size_t len1 = strlen(str1);
    const size_t len2 = strlen(str2) + 1; // includes terminating null
    
    char* const s = (char*)malloc(len1 + len2);
    memcpy(s, str1, len1);
    memcpy(s + len1, str2, len2); 
    
    return s;
}
