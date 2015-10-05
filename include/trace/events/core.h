#undef TRACE_SYSTEM
#define TRACE_SYSTEM core

#if !defined(_TRACE_CORE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CORE_H

#include <linux/tracepoint.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>

/*
 * Tracepoint for critical timings max/threshold hit.
 */
TRACE_EVENT(core_critical_timing_hit,

	TP_PROTO(unsigned long ip, unsigned long parent_ip,
			unsigned long start_ip, unsigned long flags,
			int preempt_cnt, cycles_t delta_ns),

	TP_ARGS(ip, parent_ip, start_ip, flags, preempt_cnt, delta_ns),

	TP_STRUCT__entry(
		__field(	unsigned long, ip		)
		__field(	unsigned long, parent_ip	)
		__field(	unsigned long, start_ip		)
		__field(	cycles_t, delta_ns		)
		__field(	unsigned long, flags		)
		__field(	int, preempt_cnt		)
	),

	TP_fast_assign(
		__entry->ip		= ip;
		__entry->parent_ip	= parent_ip;
		__entry->start_ip	= start_ip;
		__entry->delta_ns	= delta_ns;
		__entry->flags		= flags;
		__entry->preempt_cnt	= preempt_cnt;
	),

	TP_printk("ip=0x%lx parent_ip=0x%lx start_ip=0x%lx delta_ns=%llu irqs_disabled=%u preempt_disabled=%u in_softirq=%u in_irq=%u in_nmi=%u",
		  __entry->ip, __entry->parent_ip, __entry->start_ip,
		  (unsigned long long) __entry->delta_ns,
		  !!raw_irqs_disabled_flags(__entry->flags),
		  !!(__entry->preempt_cnt & PREEMPT_MASK),
		  !!(__entry->preempt_cnt & SOFTIRQ_MASK),
		  !!(__entry->preempt_cnt & HARDIRQ_MASK),
		  !!(__entry->preempt_cnt & NMI_MASK))
);

/*
 * Tracepoint for critical timings start/stop.
 */
DECLARE_EVENT_CLASS(core_critical_timing,

	TP_PROTO(unsigned long ip, unsigned long parent_ip,
			unsigned long flags, int preempt_cnt),

	TP_ARGS(ip, parent_ip, flags, preempt_cnt),

	TP_STRUCT__entry(
		__field(	unsigned long, ip		)
		__field(	unsigned long, parent_ip	)
		__field(	unsigned long, flags		)
		__field(	int, preempt_cnt		)
	),

	TP_fast_assign(
		__entry->ip		= ip;
		__entry->parent_ip	= parent_ip;
		__entry->flags		= flags;
		__entry->preempt_cnt	= preempt_cnt;
	),

	TP_printk("ip=0x%lx parent_ip=0x%lx irqs_disabled=%u preempt_disabled=%u in_softirq=%u in_irq=%u in_nmi=%u",
		  __entry->ip, __entry->parent_ip,
		  !!raw_irqs_disabled_flags(__entry->flags),
		  !!(__entry->preempt_cnt & PREEMPT_MASK),
		  !!(__entry->preempt_cnt & SOFTIRQ_MASK),
		  !!(__entry->preempt_cnt & HARDIRQ_MASK),
		  !!(__entry->preempt_cnt & NMI_MASK))
);

DEFINE_EVENT(core_critical_timing, core_critical_timing_start,
	TP_PROTO(unsigned long ip, unsigned long parent_ip,
		unsigned long flags, int preempt_cnt),
	TP_ARGS(ip, parent_ip, flags, preempt_cnt)
);

DEFINE_EVENT(core_critical_timing, core_critical_timing_stop,
	TP_PROTO(unsigned long ip, unsigned long parent_ip,
		unsigned long flags, int preempt_cnt),
	TP_ARGS(ip, parent_ip, flags, preempt_cnt)
);

#endif /* _TRACE_CORE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
