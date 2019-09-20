define mdb_comment
	
end

define mdb_print_fcontext_regs
	set $fcontext = (long)$arg0
	
	mdb_comment use prefix 'reg_' not $rip/$rbp, because that 'set $rip=xxx' 
	mdb_comment in gdb will try to set the register value
	set $reg_rsp = (long)($fcontext + 0x40)
	set $reg_rip = *((long *)($fcontext + 0x38))
	set $reg_rbp = *((long *)($fcontext + 0x30))
	set $reg_rbx = *((long *)($fcontext + 0x28))
	set $reg_r15 = *((long *)($fcontext + 0x20))
	set $reg_r14 = *((long *)($fcontext + 0x18))
	set $reg_r13 = *((long *)($fcontext + 0x10))
	set $reg_r12 = *((long *)($fcontext + 0x08))
	
	printf "rsp:0x%lx rip:0x%lx rbp:0x%lx rbx:0x%lx r15:0x%lx r14:0x%lx r13:0x%lx r12:0x%lx\n", $reg_rsp, $reg_rip, $reg_rbp, $reg_rbx, $reg_r15, $reg_r14, $reg_r13, $reg_r12
end

define mdb_print_bthreads
	set $ngroup = butil::ResourcePool<bthread::TaskMeta>::_ngroup.val
	set $block_groups = butil::ResourcePool<bthread::TaskMeta>::_block_groups

	mdb_comment see butil::ResourcePool<T>::describe_resources
	set $i = 0
	while $i < $ngroup
		mdb_comment bg = BlockGroup *
		set $bg = $block_groups[$i].val
		set $i = $i + 1
		set $nblock = $bg.nblock._M_i
		if $nblock > butil::RP_GROUP_NBLOCK
			set $nblock = butil::RP_GROUP_NBLOCK
		end
		
		set $j = 0
		while $j < $nblock
			mdb_comment $b = butil::ResourcePool<bthread::TaskMeta>::Block *
			set $b = $bg.blocks[$j]._M_b._M_p
			set $j = $j + 1
			set $nitem = $b.nitem
			
			set $k = 0
			while $k < $nitem
				set $addr = $b.items + $k * sizeof(bthread::TaskMeta)
				set $k = $k + 1
				set $task_meta = (bthread::TaskMeta *)($addr)
				
				if $task_meta.fn == 0 || $task_meta.stack == 0 || $task_meta.stack.context == 0
					loop_continue
				end
				
				set $task_rip = *((long *)($task_meta.stack.context + 0x38))
				if $task_rip == $task_meta.fn
					mdb_comment this is the initial 'rip' value(reference make_fcontext)
					mdb_comment this task may not in RUNNING state or in-thread context
					loop_continue
				end
				
				mdb_print_fcontext_regs $task_meta.stack.context
			end
		end
	end
end

document mdb_print_bthreads
	printf "print all bthread's registers that may be in swapout state"
end
