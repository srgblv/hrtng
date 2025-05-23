// MicrocodeExplorer.cpp from https://github.com/carbonblack/HexRaysDeob
// navigation based on block number or address has been added

#include <memory>
#include <map>
#include "warn_off.h"
#include <hexrays.hpp>
#include <graph.hpp>
#include <moves.hpp>
#include "warn_on.h"

#include "helpers.h"
#include "MicrocodeExplorer.h"

//typedef qrefcnt_t<mbl_array_t*> shared_mbl_array_t;
typedef std::shared_ptr<mbl_array_t *> shared_mbl_array_t;

const char* MicroMaturityToString(mba_maturity_t mmt) 
{ 
	switch (mmt) {
	case MMAT_ZERO: return "MMAT_ZERO";
	case MMAT_GENERATED: return "MMAT_GENERATED";
	case MMAT_PREOPTIMIZED: return "MMAT_PREOPTIMIZED";
	case MMAT_LOCOPT: return "MMAT_LOCOPT";
	case MMAT_CALLS: return "MMAT_CALLS";
	case MMAT_GLBOPT1: return "MMAT_GLBOPT1";
	case MMAT_GLBOPT2: return "MMAT_GLBOPT2";
	case MMAT_GLBOPT3: return "MMAT_GLBOPT3";
	case MMAT_LVARS: return "MMAT_LVARS";
	default: return "???"; 
	}
}

struct ida_local mblock_virtual_dumper_t : public vd_printer_t
{
	int nline;
	int serial;
	mblock_virtual_dumper_t() : nline(0), serial(0) {}
	virtual ~mblock_virtual_dumper_t() {}
	virtual void AddLine(qstring &qs) = 0;
	AS_PRINTF(3, 4) int print(int indent, const char *format, ...)
	{
		qstring buf;
		if (indent > 0)
			buf.fill(0, ' ', indent);
		va_list va;
		va_start(va, format);
		buf.cat_vsprnt(format, va);
		va_end(va);

		// ida 7.1 apparently has a problem with line prefixes, remove this color
		static const char pfx_on[] = { COLOR_ON, COLOR_PREFIX, 0 };
		static const char pfx_off[] = { COLOR_OFF, COLOR_PREFIX, 0 };
		buf.replace(pfx_on, "");
		buf.replace(pfx_off, "");

		AddLine(buf);
		return (int)buf.length();
	}
};

struct ida_local mblock_qstring_dumper_t : public mblock_virtual_dumper_t
{
	qstring qStr;
	virtual ~mblock_qstring_dumper_t() {}
	virtual void AddLine(qstring &qs)
	{
		qStr.append(qs);
	}
};

struct ida_local mblock_dumper_t : public mblock_virtual_dumper_t
{
	strvec_t lines;
	std::map<int, int> block2line;
	std::map<ea_t, int> ea2line;
	virtual ~mblock_dumper_t() {}
	virtual void AddLine(qstring &qs)
	{
		const char* pLine = qs.c_str();
		const char* pDot = qstrchr(pLine, '.');
		if (pDot) {
			int nBlock = atoi(pLine);
			if(pDot[1] == ' ' && pDot[2] == '0' && !strneq(pDot + 3, " " SCOLOR_ON SCOLOR_RPTCMT ";", 4) && block2line.find(nBlock) == block2line.end())
				block2line[nBlock] = (int)lines.size();
		}
		const char* pSC = qstrchr(pLine, ';');
		if (pSC) {
			long long a = strtoll(pSC + 2, NULL, 16);
			ea_t ea = (ea_t)a;
			if (ea != 0 && ea2line.find(ea) == ea2line.end())
				ea2line[ea] = (int)lines.size();
		}
		lines.push_back(simpleline_t(qs));
	}
};

struct ida_local sample_info_t
{
	TWidget *cv;
	mblock_dumper_t md;
	shared_mbl_array_t mba;
	qstring name;
	sample_info_t() : cv(NULL), mba(NULL) {}
};

class ida_local MicrocodeInstructionGraph
{
public:
	qstring tmp;            // temporary buffer for grcode_user_text
	qstrvec_t m_BlockText;
	intvec_t m_EdgeColors;
	edgevec_t m_Edges;
	int m_NumBlocks;

	void Clear()
	{
		m_BlockText.clear();
		m_EdgeColors.clear();
		m_Edges.clear();
		m_NumBlocks = 0;
	}

	void Build(minsn_t *top)
	{
		Clear();
		Insert(top, -1);
	}

protected:
	void AddEdge(int iSrc, int iDest, int iPos)
	{
		if (iSrc < 0 || iDest < 0)
			return;

		m_Edges.push_back(edge_t(iSrc, iDest));
		m_EdgeColors.push_back(iPos);
	}

	int GetIncrBlockNum()
	{
		return m_NumBlocks++;
	}

	int Insert(minsn_t *ins, int /*iParent*/)
	{
		qstring qStr;
		ins->print(&qStr);
		m_BlockText.push_back(qStr);

		int iThisBlock = GetIncrBlockNum();

		Insert(ins->l, iThisBlock, 0);
		Insert(ins->r, iThisBlock, 1);
		Insert(ins->d, iThisBlock, 2);

		return iThisBlock;
	}
	int Insert(mop_t &op, int iParent, int iPos)
	{
		if (op.t == mop_z)
			return -1;

		qstring qStr;
		op.print(&qStr);
		m_BlockText.push_back(qStr);

		int iThisBlock = GetIncrBlockNum();
		AddEdge(iParent, iThisBlock, iPos);

		switch (op.t)
		{
		case mop_d: // result of another instruction
		{
			int iDestBlock = Insert(op.d, iThisBlock);
			AddEdge(iThisBlock, iDestBlock, 0);
			break;
		}
		case mop_f: // list of arguments
			for (int i = 0; i < op.f->args.size(); ++i)
				Insert(op.f->args[i], iThisBlock, i);
			break;
		case mop_p: // operand pair
		{
			Insert(op.pair->lop, iThisBlock, 0);
			Insert(op.pair->hop, iThisBlock, 1);
			break;
		}
		case mop_a: // result of another instruction
		{
			Insert(*op.a, iThisBlock, 0);
			break;
		}
		}
		return iThisBlock;
	}
};

static ssize_t idaapi migr_callback(void *ud, int code, va_list va);

class ida_local MicrocodeInstructionGraphContainer
{
protected:
	TWidget * m_TW;
	graph_viewer_t *m_GV;
	qstring m_Title;
	qstring m_GVName;

public:
	MicrocodeInstructionGraph m_MG;
	MicrocodeInstructionGraphContainer() : m_TW(NULL), m_GV(NULL) {};

	bool Display(minsn_t *top, sample_info_t *si)
	{
		mbl_array_t *mba = *si->mba;
		m_MG.Build(top);

		m_Title.cat_sprnt("Microinstruction Graph - %a[%s]/%a", mba->entry_ea, si->name.c_str(), top->ea);
		m_TW = create_empty_widget(m_Title.c_str());
		netnode id;
		id.create();

		m_GVName.cat_sprnt("microins_%a_%s_%a", mba->entry_ea, si->name.c_str(), top->ea);
		m_GV = create_graph_viewer(m_GVName.c_str(), id, migr_callback, this, 0, m_TW);
		activate_widget(m_TW, true);
		display_widget(m_TW, 0);
		viewer_fit_window(m_GV);
		return true;
	}
};

static ssize_t idaapi migr_callback(void *ud, int code, va_list va)
{
	MicrocodeInstructionGraphContainer *gcont = (MicrocodeInstructionGraphContainer *)ud;
	MicrocodeInstructionGraph *microg = &gcont->m_MG;
	bool result = false;

	switch (code)
	{
#if IDA_SDK_VERSION < 760
	case grcode_user_gentext:
		result = true;
		break;
#endif //IDA_SDK_VERSION < 760

		// refresh user-defined graph nodes and edges
	case grcode_user_refresh:
		// in:  interactive_graph_t *g
		// out: success
	{
		interactive_graph_t* mg = va_arg(va, interactive_graph_t*);

		// we have to resize
		mg->resize(microg->m_NumBlocks);

		for (auto &it : microg->m_Edges)
			mg->add_edge(it.src, it.dst, NULL);

		result = true;
	}
	break;

	// retrieve text for user-defined graph node
	case grcode_user_text:
		//interactive_graph_t *g
		//      int node
		//      const char **result
		//      bgcolor_t *bg_color (maybe NULL)
		// out: must return 0, result must be filled
		// NB: do not use anything calling GDI!
	{
		va_arg(va, interactive_graph_t*);
		int node = va_arg(va, int);
		const char **text = va_arg(va, const char **);

		microg->tmp = microg->m_BlockText[node];
		*text = microg->tmp.begin();
		result = true;
	}
	break;
	}
	return (int)result;
}

static ssize_t idaapi mgr_callback(void *ud, int code, va_list va);

class ida_local MicrocodeGraphContainer
{
public:
	shared_mbl_array_t m_MBA;
	mblock_qstring_dumper_t m_MQD;
	qstring m_Title;
	qstring m_GVName;
	qstring tmp;
	MicrocodeGraphContainer(shared_mbl_array_t mba) : m_MBA(mba) {};
	void Display(sample_info_t *si)
	{
		mbl_array_t *mba = *si->mba;
		m_Title.cat_sprnt("Microcode Graph - %a[%s]", mba->entry_ea, si->name.c_str());

		TWidget *tw = create_empty_widget(m_Title.c_str());
		netnode id;
		id.create();

		m_GVName.cat_sprnt("microblkgraph_%a_%s", mba->entry_ea, si->name.c_str());
		graph_viewer_t *gv = create_graph_viewer(m_GVName.c_str(), id, mgr_callback, this, 0, tw);
		activate_widget(tw, true);
		display_widget(tw, 0);
		viewer_fit_window(gv);
		return;
	}
};

static ssize_t idaapi mgr_callback(void *ud, int code, va_list va)
{
	MicrocodeGraphContainer *gcont = (MicrocodeGraphContainer *)ud;
	mbl_array_t *mba = *gcont->m_MBA;
	bool result = false;

	switch (code)
	{
#if IDA_SDK_VERSION < 760
	case grcode_user_gentext:
		result = true;
		break;
#endif // IDA_SDK_VERSION < 760

		// refresh user-defined graph nodes and edges
	case grcode_user_refresh:
		// in:  interactive_graph_t *g
		// out: success
	{
		interactive_graph_t*mg = va_arg(va, interactive_graph_t*);

		// we have to resize
		mg->resize(mba->qty);

		for (int i = 0; i < mba->qty; ++i)
			for (auto dst : mba->get_mblock(i)->succset)
				mg->add_edge(i, dst, NULL);

		result = true;
	}
	break;

	// retrieve text for user-defined graph node
	case grcode_user_text:
		//interactive_graph_t *g
		//      int node
		//      const char **result
		//      bgcolor_t *bg_color (maybe NULL)
		// out: must return 0, result must be filled
		// NB: do not use anything calling GDI!
	{
		va_arg(va, interactive_graph_t*);
		int node = va_arg(va, int);
		const char **text = va_arg(va, const char **);

		gcont->m_MQD.qStr.clear();
		mba->get_mblock(node)->print(gcont->m_MQD);
		*text = gcont->m_MQD.qStr.begin();
		result = true;
	}
	break;
	}
	return (int)result;
}

static void cv_jump(sample_info_t* si)
{
#if IDA_SDK_VERSION >= 740
	int nBlock = -1;
	qstring buf;
	tag_remove(&buf, get_custom_viewer_curline(si->cv, false));
	lochist_entry_t hist;
	get_custom_viewer_location(&hist, si->cv, false);
	const char* pLine = buf.c_str();
	const char* pAt = qstrchr(pLine, '@');
	if (pAt) {
		nBlock = atoi(pAt + 1);
	} else {
		pAt = pLine + hist.rinfo.pos.cx;
		while (pAt > pLine && qisdigit(*(pAt - 1)))
			--pAt;
		if (qisdigit(*pAt))
			nBlock = atoi(pAt);
	}
	if (nBlock != -1 && si->md.block2line.find(nBlock) != si->md.block2line.end()) {
		simpleline_place_t* newplace = (simpleline_place_t*)hist.place()->clone();
		newplace->n = si->md.block2line[nBlock];
		hist.set_place(newplace);
		custom_viewer_jump(si->cv, hist, CVNF_JUMP);
	}
#endif //IDA_SDK_VERSION >= 740
}

static bool idaapi ct_dblclick(TWidget* cv, int shift, void* ud)
{
	sample_info_t* si = (sample_info_t*)ud;
	cv_jump(si);
	return true;
}

bool InsGraphCheck(sample_info_t* si, minsn_t** minsn)
{
	qstring buf;
	tag_remove(&buf, get_custom_viewer_curline(si->cv, false));
	const char* pLine = buf.c_str();
	const char* pDot = qstrchr(pLine, '.');
	if (pDot == NULL)
		return false;

	int nBlock = atoi(pLine);
	int nSerial = atoi(pDot + 1);
	mbl_array_t* mba = *si->mba;

	if (nBlock > mba->qty)
		return false;

	mblock_t* blk = mba->get_mblock(nBlock);
	*minsn = blk->head;
	int i;
	for (i = 0; i < nSerial; ++i) {
		*minsn = (*minsn)->next;
		if (*minsn == NULL)
			break;
	}

	if (*minsn == NULL)
		return false;

	return true;
}

static bool idaapi ct_keyboard(TWidget * /*v*/, int key, int shift, void *ud)
{
	sample_info_t* si = (sample_info_t*)ud;
	if (shift == 0) {
		switch (key)
		{
#if IDA_SDK_VERSION >= 740
		case 'G':
			{
				qstring nBlock_or_addr;
				if (!ask_str(&nBlock_or_addr, HIST_SEG, "[hrt] Go to block number or address..."))
					return false;
				
				lochist_entry_t hist;
				get_custom_viewer_location(&hist, si->cv, false);
				simpleline_place_t* newplace = (simpleline_place_t*)hist.place()->clone();

				int nBlock = atoi(nBlock_or_addr.c_str());
				ea_t ea = BADADDR;
				if(atoea(&ea, nBlock_or_addr.c_str()) && is_mapped(ea) && si->md.ea2line.find(ea) != si->md.ea2line.end()) {
					newplace->n = si->md.ea2line[ea];
				} else if (si->md.block2line.find(nBlock) != si->md.block2line.end()) {
					newplace->n = si->md.block2line[nBlock];
				}
				hist.set_place(newplace);
				custom_viewer_jump(si->cv, hist, CVNF_JUMP);
				return true;
			}
			break;
#endif //IDA_SDK_VERSION >= 740
		case 'I':
		{
			if (si->mba == NULL)
				return false;
			minsn_t* minsn;
			if (InsGraphCheck(si, &minsn)) {
				MicrocodeInstructionGraphContainer* mcg = new MicrocodeInstructionGraphContainer;
				return mcg->Display(minsn, si);
			}
		}
		case 'M':
			if (si->mba != NULL) {
				MicrocodeGraphContainer* mgc = new MicrocodeGraphContainer(si->mba);
				mgc->Display(si);
				return true;
			}
			break;
		case IK_RETURN:
			cv_jump(si);
			return true;
		default:
			break;
		}
	}
	return false;
}

static const custom_viewer_handlers_t handlers(
	ct_keyboard,
	NULL, // popup
	NULL, // mouse_moved
	NULL, // click
	ct_dblclick,
	NULL, // ct_curpos
	NULL, // close
	NULL, // help
	NULL);// adjust_place

struct ida_local microcodegraph_ah_t : public action_handler_t
{
	sample_info_t* si;
	microcodegraph_ah_t(sample_info_t* _si) : si(_si) {}
	virtual int idaapi activate(action_activation_ctx_t*) override
	{
		MicrocodeGraphContainer* mgc = new MicrocodeGraphContainer(si->mba);
		mgc->Display(si);
		return 1;
	}
	virtual action_state_t idaapi update(action_update_ctx_t*) override
	{
		return AST_ENABLE_ALWAYS;
	}
};

struct ida_local microinsngraph_ah_t : public action_handler_t
{
	sample_info_t* si;
	minsn_t* minsn;
	microinsngraph_ah_t(sample_info_t* _si, minsn_t* _minsn) : si(_si), minsn(_minsn) {}
	virtual int idaapi activate(action_activation_ctx_t*) override
	{
		MicrocodeInstructionGraphContainer* mcg = new MicrocodeInstructionGraphContainer;
		mcg->Display(minsn, si);
		return 1;
	}
	virtual action_state_t idaapi update(action_update_ctx_t*) override
	{
		return AST_ENABLE_ALWAYS;
	}
};


static ssize_t idaapi ui_callback(void *ud, int code, va_list va)
{
	sample_info_t *si = (sample_info_t *)ud;
	switch (code)
	{
	case ui_widget_invisible:
	{
		TWidget *f = va_arg(va, TWidget *);
		if (f == si->cv) {
			delete si;
			unhook_from_notification_point(HT_UI, ui_callback);
		}
	}
	break;
	case ui_populating_widget_popup:
	{
		TWidget* f = va_arg(va, TWidget*);
		TPopupMenu* p = va_arg(va, TPopupMenu*);
		if (f == si->cv && si->mba != NULL) {
			action_desc_t desc = DYNACTION_DESC_LITERAL("Show Microcode Graph", new microcodegraph_ah_t(si), "M", nullptr, -1);
			attach_dynamic_action_to_popup(f, p, desc, nullptr, 0);

			minsn_t* minsn;
			if (InsGraphCheck(si, &minsn)) {
				action_desc_t desc = DYNACTION_DESC_LITERAL("Show Microinstruction Graph", new microinsngraph_ah_t(si, minsn), "I", nullptr, -1);
				attach_dynamic_action_to_popup(f, p, desc, nullptr, 0);
			}
		}
	}
	break;
	}
	return 0;
}

void showMicrocodeExplorer(mbl_array_t* mba, bool keepMba, const char* name)
{
	qstring title;
	TWidget* widget = NULL;
	int cnt = 0;
	do {
		qstring suffix;
		if (cnt++ > 0)
			suffix.sprnt(" (%d)", cnt);
		if (cnt > 3)
			return;
		title.sprnt("Microcode - %a - %s%s", mba->entry_ea, name, suffix.c_str());
		widget = find_widget(title.c_str());
	} while (widget);
	sample_info_t* si = new sample_info_t;
	si->name = name;
	if(keepMba)
		si->mba = std::make_shared<mbl_array_t*>(mba);

	mba->print(si->md);

	simpleline_place_t s1;
	simpleline_place_t s2((int)si->md.lines.size() - 1);


	si->cv = create_custom_viewer(
		title.c_str(), // title
		&s1, // minplace
		&s2, // maxplace
		&s1, // curplace
		NULL, // renderer_info_t *rinfo
		&si->md.lines, // ud
		&handlers, // cvhandlers
		si, // cvhandlers_ud
		NULL); // parent

	hook_to_notification_point(HT_UI, ui_callback, si);
	display_widget(si->cv, WOPN_DP_TAB | WOPN_NOT_CLOSED_BY_ESC, "IDA View-A");
}

void ShowMicrocodeExplorer(mbl_array_t* mba, const char* name)
{
	showMicrocodeExplorer(mba, false, name);
}

//-------------------------------------------------------------------------

const char *matLevels[] =
{
	"MMAT_GENERATED",
	"MMAT_PREOPTIMIZED",
	"MMAT_LOCOPT",
	"MMAT_CALLS",
	"MMAT_GLBOPT1",
	"MMAT_GLBOPT2",
	"MMAT_GLBOPT3",
	"MMAT_LVARS"
};

mba_maturity_t AskDesiredMaturity()
{
	const char dlgText[] =
		"Select maturity level\n"
		"<Desired ~m~aturity level:b:0:::>\n";

	qstrvec_t opts;
	for (int i = 0; i < qnumber(matLevels); ++i)
		opts.push_back(matLevels[i]);

	static int sel = 0;
	int ret = ask_form(dlgText, &opts, &sel);

	if (ret > 0)
		return (mba_maturity_t)((int)MMAT_GENERATED + sel);
	return MMAT_ZERO;
}


//-------------------------------------------------------------------------
ACT_DECL(show_microcode_explorer, AST_ENABLE_ALW)

void registerMicrocodeExplorer()
{
	COMPAT_register_and_attach_to_menu("View/Open subviews/Generate pseudocode", ACT_NAME(show_microcode_explorer), "[hrt] Microcode Explorer...", NULL, SETMENU_APP, &show_microcode_explorer, &PLUGIN);
}

void unregisterMicrocodeExplorer()
{
	detach_action_from_menu("View/Open subviews/[hrt] Microcode Explorer...", ACT_NAME(show_microcode_explorer));
	unregister_action(ACT_NAME(show_microcode_explorer));
}

ACT_DEF(show_microcode_explorer)
{
	func_t* pfn = get_func(get_screen_ea());
	if (pfn == NULL) {
		warning("Please position the cursor within a function");
		return 0;
	}

	mba_maturity_t mmat = AskDesiredMaturity();
	if (mmat == MMAT_ZERO)
		return 0;

	hexrays_failure_t hf;
	mbl_array_t* mba = gen_microcode(pfn, &hf, NULL, DECOMP_NO_CACHE, mmat);
	if (mba == NULL) {
		warning("#error \"%a: %s", hf.errea, hf.desc().c_str());
		return 0;
	}

	showMicrocodeExplorer(mba, true, MicroMaturityToString(mmat));
	return 1;
}
