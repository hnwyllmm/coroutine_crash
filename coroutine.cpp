#include <elf.h>
#include <sys/procfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>

#include <string>
#include <memory>
#include <deque>
#include <fstream>

#define ALIGN(x, a) (((x)+(a)-1UL)&~((a)-1UL))

#define error_msg_and_die(fmt, ...)  	\
	do {								\
		printf(fmt, ##__VA_ARGS__);		\
		exit(1);						\
	} while (0)

int readn(int fd, void *buf, size_t size)
{
    char *tmp = (char *) buf;
    while (size > 0)
    {
        const ssize_t ret = read(fd, tmp, size);
        if (ret > 0)
        {
            tmp += ret;
            size -= ret;
            continue;
        }

        if (0 == ret)
            return -1; // end of file

        const int err = errno;
        if (EAGAIN != err && EINTR != err)
            return err;
    }
    return 0;
}

int writen(int fd, const void *buf, size_t size)
{
    const char *tmp = (const char *) buf;
    while (size > 0)
    {
        const ssize_t ret = ::write(fd, tmp, size);
        if (ret >= 0)
        {
            tmp += ret;
            size -= ret;
            continue;
        }

        const int err = errno;
        if (EAGAIN != err && EINTR != err)
            return err;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

/* DWARF column numbers for x86_64: */
#define RAX     0
#define RDX     1
#define RCX     2
#define RBX     3
#define RSI     4
#define RDI     5
#define RBP     6
#define RSP     7
#define R8      8
#define R9      9
#define R10     10
#define R11     11
#define R12     12
#define R13     13
#define R14     14
#define R15     15
#define RIP     16

#define REG_NUM 17

int reg_name_to_number(const char *name)
{
	static const char *regnames[] = {
		"RAX", "RDX", "RCX", "RBX",
		"RSI", "RDI", "RBP", "RSP", 
		"R8",  "R9",  "R10", "R11", 
		"R12", "R13", "R14", "R15", 
		"RIP"
	};

	for (int i = 0; i < sizeof(regnames)/sizeof(regnames[0]); i++)
	{
		if (strcasecmp(regnames[i], name) == 0)
			return i;
	}
	return -1;
}

// 这段代码从libunwind中复制
static const int8_t remap_regs[] =
    {
      [RAX]    = offsetof(struct user_regs_struct, rax) / sizeof(long),
      [RDX]    = offsetof(struct user_regs_struct, rdx) / sizeof(long),
      [RCX]    = offsetof(struct user_regs_struct, rcx) / sizeof(long),
      [RBX]    = offsetof(struct user_regs_struct, rbx) / sizeof(long),
      [RSI]    = offsetof(struct user_regs_struct, rsi) / sizeof(long),
      [RDI]    = offsetof(struct user_regs_struct, rdi) / sizeof(long),
      [RBP]    = offsetof(struct user_regs_struct, rbp) / sizeof(long),
      [RSP]    = offsetof(struct user_regs_struct, rsp) / sizeof(long),
	  [R8]     = offsetof(struct user_regs_struct, r8)  / sizeof(long),
	  [R9]     = offsetof(struct user_regs_struct, r9)  / sizeof(long),
      [R10]    = offsetof(struct user_regs_struct, r10) / sizeof(long),
      [R11]    = offsetof(struct user_regs_struct, r11) / sizeof(long),
      [R12]    = offsetof(struct user_regs_struct, r12) / sizeof(long),
      [R13]    = offsetof(struct user_regs_struct, r13) / sizeof(long),
      [R14]    = offsetof(struct user_regs_struct, r14) / sizeof(long),
      [R15]    = offsetof(struct user_regs_struct, r15) / sizeof(long),
      [RIP]    = offsetof(struct user_regs_struct, rip) / sizeof(long),
    };

/**
 * 解析寄存器信息
 * 格式  rsp:0xabcd rip:0x1234  rax:0x1111
 */
int parse_regs(char *line, elf_gregset_t regset)
{
  /* format : RSP:0x123 RIP:0x3123 */
  char *   sep_pair;
  char *   sep_field;
  int      reg_number;
  intptr_t reg_value;
  char *   endptr;

  sep_pair = strchr(line, ':');
  while (sep_pair)
  {
	*sep_pair = '\0';
	reg_number = reg_name_to_number(line);
	*sep_pair = ':';
	if (reg_number < 0)
	  return -1;

	sep_field = strchr(sep_pair + 1, ' ');
	reg_value = strtol(sep_pair + 1, &endptr, 0);
	if (endptr != sep_field && *endptr != '\0' && *endptr != '\r' && *endptr != '\n')
	  return -1;

	if (reg_number >= sizeof(remap_regs)/sizeof(remap_regs[0]))
      return -1;

    reg_number = remap_regs[reg_number];
	regset[reg_number] = reg_value;

	if (sep_field)
	  line = sep_field + 1;
	else
		break;

	if (*line == '\0')
		break;

	sep_pair = strchr(line, ':');
  }

  return 0;
}

static void dumphex(const char data[], size_t size, std::string &hex)
{
    hex.resize(size * 2);
    for (size_t i = 0; i < size; i++)
    {
        char c = data[i];
        char h = (c >> 4) & 0x0F;
        char l = c & 0x0F;

        if (h >= 0x0A)
            h = h + 'A' - 10;
        else
            h = h + '0';
        if (l >= 0x0A)
            l = l + 'A' - 10;
        else
            l = l + '0';

        hex[i * 2] = h;
        hex[i * 2 + 1] = l;
    }
}

////////////////////////////////////////////////////////////////////////////////

/**
 * 计算NOTE program header实际大小
 * program header实际包含name和desc两个字段
 */
inline static uint64_t note_size(Elf64_Nhdr *hdr)
{
	return sizeof(*hdr) + ALIGN(hdr->n_namesz, 4) + ALIGN(hdr->n_descsz, 4);
}

/**
 * 判断hdr是否还在有效范围内
 * @param hdr header起始地址
 * @param end 整个NOTE program header最后的内存地址
 * @note 就是判断hdr本身结构体大小加上自己所占内存有没有超出范围
 */
inline static bool note_fits(Elf64_Nhdr *hdr, char *end)
{
	char *const begin = (char *)hdr;
	const auto size = end - begin;
	if (size < sizeof(*hdr))
		return false;
	return size >= note_size(hdr);
}

/**
 * 获取note header的desc字段内存起始地址
 */
char *note_desc(Elf64_Nhdr *hdr)
{
	return (char *)hdr + sizeof(*hdr) + ALIGN(hdr->n_namesz, 4);
}

/**
 * 表示一个协程/线程在core中的数据，目前只有寄存器信息
 */
struct coroutine_t
{
	elf_gregset_t  regset;
};

class core_handler_t
{
public:
	core_handler_t(const char *corefile) : m_corefile(corefile)
	{
		if (!corefile)
			error_msg_and_die("invalid parameter `corefile`\n");
	}

	bool init()
	{
		// read elf header and check it
		m_core_fd = open(m_corefile.c_str(), O_RDWR);
		if (m_core_fd < 0)
		{
			printf("open file fail: %s. msg %s\n", m_corefile.c_str(), strerror(errno));
			return false;
		}

		if (0 != readn(m_core_fd, &m_elf_header, sizeof(m_elf_header)))
		{
			printf("read elf header fail. msg %s\n", strerror(errno));
			return false;
		}

		if (m_elf_header.e_ident[EI_CLASS] != ELFCLASS64)
		{
			printf("note a valid elf file or not a 64-bit elf file\n");
			return false;
		}

		// check the endian 
		//if (WE_ARE_LITTLE_ENDIAN != (elf_header32.e_ident[EI_DATA] == ELFDATA2LSB))
		
		if (sizeof(m_elf_header.e_entry) > sizeof(off_t))
		{
			printf("cannot process elf. only 64-bit file can be processed\n");
			return false;
		}

		// read program headers to find the PT_NOTE header
		if (m_elf_header.e_phoff != lseek(m_core_fd, m_elf_header.e_phoff, SEEK_SET))
		{
			printf("seek to program header fail, offset %lu. msg %s\n",
				m_elf_header.e_phoff, strerror(errno));
			return false;
		}

		/// find the NOTE program header
		const uint32_t phdr_number = m_elf_header.e_phnum;
		m_note_phdr.p_type = PT_NULL;
		for (uint32_t i = 0; i < phdr_number; i++)
		{
			// Now, I suppose that only on PT_NOTE exists
			if (0 != readn(m_core_fd, &m_note_phdr, sizeof(m_note_phdr)))
			{
				printf("read program header fail. msg %s\n", strerror(errno));
				return false;
			}

			if (m_note_phdr.p_type == PT_NOTE)
			{
				const off_t curoff = lseek(m_core_fd, 0, SEEK_CUR);
				if (curoff == off_t(-1))
				{
					printf("[find note program header] cannot get current offset. msg %s\n",
						strerror(errno));
				}
				m_note_phdr_offset = curoff;
				break;
			}
		}

		if (m_note_phdr.p_type != PT_NOTE || m_note_phdr_offset == off_t(-1))
		{
			printf("cannot find type PT_NOTE program header\n");
			return false;
		}

		// read all segment data in PT_NOTE program segment
		char *const note_segment = new (std::nothrow) char[m_note_phdr.p_filesz];
		if (!note_segment)
		{
			printf("alloc memory fail. size %lu\n", m_note_phdr.p_filesz);
			return false;
		}

		std::unique_ptr<char> note_segment_guard(note_segment);

		if (lseek(m_core_fd, m_note_phdr.p_offset, SEEK_SET) != m_note_phdr.p_offset)
		{
			printf("cannot seek to note segment, offset %lu. msg %s\n", 
				m_note_phdr.p_offset, strerror(errno));
			return false;
		}

		if (0 != readn(m_core_fd, note_segment, m_note_phdr.p_filesz))
		{
			printf("read note segment fail, file size %lu. msg %s\n",
				m_note_phdr.p_filesz, strerror(errno));
			return false;
		}

		char * const note_end = note_segment + m_note_phdr.p_filesz;

		// iterate the note program segment to find the threads
		uint64_t thread_number = 0;
		Elf64_Nhdr *note_hdr = (Elf64_Nhdr *)note_segment;
		char *first_note_addr  = nullptr;
		char *second_note_addr = nullptr;
		__pid_t max_pid = 0;
		while (note_fits(note_hdr, note_end))
		{
			if (note_hdr->n_type == NT_PRSTATUS) // thread
			{
				thread_number++;

				prstatus_t *prstatus = (prstatus_t *)note_desc(note_hdr);
				if (prstatus->pr_pid > max_pid)
					max_pid = prstatus->pr_pid;

				// record the first thread information data
				if (!first_note_addr)
					first_note_addr = (char *)note_hdr;
				else if (!second_note_addr)
					second_note_addr = (char *)note_hdr;
			}

			note_hdr = (Elf64_Nhdr *)((char *)note_hdr + note_size(note_hdr) );
		}

		const off_t filesize = lseek(m_core_fd, 0, SEEK_END);
		if (off_t(-1) == filesize)
		{
			printf("lseek to end fail. msg %s\n", strerror(errno));
			return false;
		}

		m_note_segment.swap(note_segment_guard);
		m_max_pid = max_pid;
		m_first_thread_addr = first_note_addr;
		m_first_thread_size = second_note_addr - first_note_addr;
		printf("file size %lu\nnote program header: offset %lu\nnote program: filesz %lu, offset %lu\n"
			   "max pid %d, thread number %lu\nfirst thread size %ld\n",
			filesize, m_note_phdr_offset, m_note_phdr.p_filesz, m_note_phdr.p_offset, 
			max_pid, thread_number, m_first_thread_size);

		return max_pid != 0;
	}

	bool add_thread(char *reg_str)
	{
		coroutine_t coroutine;
		const int ret = parse_regs(reg_str, coroutine.regset);
		if (ret == -1)
			printf("parse reg fail: %s\n",reg_str); 
		else
		{
			m_threads_extra.push_back(coroutine);
			// printf("got a thread with regs: %s\n", reg_str);
		}
		return 0 == ret;
	}

	bool add_thread_file(const char *reg_file)
	{
		std::fstream fstream(reg_file);
		if (!fstream.is_open())
		{
			printf("cannot open file: %s\n", reg_file);
			return false;
		}
		std::string line;
		std::getline(fstream, line);
		while (fstream.good())
		{
			if (!line.empty())
				add_thread((char *)line.c_str());

			std::getline(fstream, line);
		}
		return true;
	}

	bool flush_to_file()
	{
		if (m_threads_extra.empty())
		{
			printf("no thread to append\n");
			return true;
		}

		Elf64_Phdr phdr;
		//std::string hex;
		std::unique_ptr<char> thread_info(new (std::nothrow) char[m_first_thread_size]);

		if (!thread_info)
		{
			printf("alloc memory fail. size %lu\n", m_first_thread_size);
			return false;
		}

		const off_t source_filesize = lseek(m_core_fd, 0, SEEK_END);
		if (off_t(-1) == source_filesize)
		{
			printf("seek to file end fail. msg %s\n", strerror(errno));
			return false;
		}

		printf("write source note segment data with size %lu\n", m_note_phdr.p_filesz);
		if (0 != writen(m_core_fd, m_note_segment.get(), m_note_phdr.p_filesz))
		{
			printf("append note segment information fail. size %lu, msg %s\n",
				m_note_phdr.p_filesz, strerror(errno));
			goto rollback;
		}

		//dumphex(m_first_thread_addr, m_first_thread_size, hex);
		//printf("%s\n", hex.c_str());
		memcpy(thread_info.get(), m_first_thread_addr, m_first_thread_size);

		
		printf("append extra threads, number =%d\n", m_threads_extra.size());
		for (auto & coroutine : m_threads_extra)
		{
			prstatus_t *status = (prstatus_t *)(note_desc((Elf64_Nhdr *)thread_info.get()));
			status->pr_pid = ++m_max_pid;
			memcpy(status->pr_reg, coroutine.regset, sizeof(status->pr_reg));

			if (0 != writen(m_core_fd, thread_info.get(), m_first_thread_size))
			{
				printf("append note segment information fail. size %lu, msg %s\n",
					m_first_thread_size, strerror(errno));
				goto rollback;
			}
		}
		
		printf("modify the note program header information. offset=%lu, size=%lu\n",
			m_note_phdr_offset, sizeof(phdr));

		phdr = m_note_phdr;
		phdr.p_offset = source_filesize;
		phdr.p_filesz += m_first_thread_size * m_threads_extra.size();
		if (-1 == lseek(m_core_fd, m_note_phdr_offset, SEEK_SET))
		{
			printf("seek to note program header fail. offset %lu, msg %s\n",
					m_note_phdr_offset, strerror(errno));
			goto rollback;
		}
		if (0 != writen(m_core_fd, &phdr, sizeof(phdr)))
		{
			printf("append note segment information fail. size %lu, msg %s\n",
				sizeof(phdr), strerror(errno));
			goto rollback;
		}
		return true;

rollback:
		if ( 0 != ftruncate(m_core_fd, source_filesize))
		{
			printf("rollback fail. truncate file to %lu. msg %s\n",
				source_filesize, strerror(errno));
			return false;
		}

		if (-1 == lseek(m_core_fd, m_note_phdr_offset, SEEK_SET))
		{
			printf("seek to note program header fail. offset %lu, msg %s\n",
					m_note_phdr_offset, strerror(errno));
			return false;
		}
		if (0 != writen(m_core_fd, &m_note_phdr, sizeof(m_note_phdr)))
		{
			printf("append note segment information fail. size %lu, msg %s\n",
				sizeof(m_note_phdr), strerror(errno));
		}

		return false;
	}

private:
	std::string   			m_corefile;

	int           			m_core_fd;

	Elf64_Ehdr    			m_elf_header;	  // elf header
	Elf64_Phdr    			m_note_phdr;      // PT_NOTE program header
	off_t         			m_note_phdr_offset = off_t(-1);  // PT_NOTE program header在core文件中的偏移量

	std::unique_ptr<char>  	        m_note_segment;      // note 整个内存
	char *        			m_first_thread_addr; // core文件中第一个线程的起始内存位置
	uint64_t      			m_first_thread_size; // core文件中第一个线程的内存大小
	__pid_t       			m_max_pid;           // 最大的pid值(LWP)，如果LWP值相同，gdb会认为同一个线程，将会不展示

	std::deque<coroutine_t>         m_threads_extra;     // 协程信息解析出来就放在这里
};

int main(int argc, char *argv[])
{
	const char *corefile = argv[1];
	const char *regs_file = argv[2];
	printf("core file %s\n", corefile);

	core_handler_t handler(corefile);
	if (!handler.init())
		return 1;

	handler.add_thread_file(regs_file);
	handler.flush_to_file();
	return 0;
}

