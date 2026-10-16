/*
 * hidparse.c - HID report descriptor parser and the HidP_* report access API
 * for the Win95 OSR2.1 USB HID port.  Interface: 98DDK hidpi.h / hidpddi.h.
 *
 * Model
 *   A report descriptor is parsed into one HIDP_PREPARSED_DATA per top-level
 *   (application) collection.  Each main item becomes one or more FIELDs:
 *     button  - variable item, 1 bit per control, or an array item
 *     value   - variable item wider than 1 bit
 *     padding - constant items are skipped (they only advance the bit position)
 *   The preparsed data is one allocation:
 *     header | link collection nodes | fields | usages | report lengths
 */
#include "hpport.h"

#define HP_MAGIC        0x50646948UL        /* 'HidP' */
#define HP_NONE         0xFFFF

/* main item flags */
#define MF_CONST        0x001
#define MF_VARIABLE     0x002
#define MF_RELATIVE     0x004
#define MF_NULLSTATE    0x040

#define FK_BUTTON       0       /* variable, 1 bit each, single usage or range */
#define FK_ARRAY        1       /* array item: slot value selects a usage      */
#define FK_VALUE        2       /* variable, wider than 1 bit                  */

typedef struct _HP_USAGE {
    USAGE   Page;
    USAGE   Usage;
} HP_USAGE;

typedef struct _HP_NODE {
    USAGE   Usage;
    USAGE   Page;
    USHORT  Parent;
    USHORT  NumChildren;
    USHORT  NextSibling;
    USHORT  FirstChild;
    UCHAR   Type;               /* collection type */
    UCHAR   Pad;
} HP_NODE;

typedef struct _HP_FIELD {
    UCHAR   Type;               /* HIDP_REPORT_TYPE */
    UCHAR   Kind;               /* FK_* */
    UCHAR   ReportId;
    UCHAR   IsRange;
    USHORT  Flags;              /* main item flags */
    USHORT  Node;               /* link collection (local index) */
    ULONG   BitPos;             /* in the report data, after the id byte */
    USHORT  BitSize;
    USHORT  Count;              /* slots in this field */
    USAGE   Page;
    USAGE   UsageMin;           /* range, or the single usage */
    USAGE   UsageMax;
    ULONG   ListOffset;         /* usages[] index, arrays with a usage list */
    USHORT  ListCount;
    USHORT  DataIndexMin;
    USHORT  DataIndexMax;
    USHORT  StringMin, StringMax;
    USHORT  DesMin, DesMax;
    LONG    LogMin, LogMax;
    LONG    PhysMin, PhysMax;
    ULONG   Units, UnitsExp;
} HP_FIELD;

typedef struct _HP_REPLEN {
    UCHAR   Id;
    UCHAR   Type;
    USHORT  Bytes;              /* data bytes, without the id byte */
} HP_REPLEN;

typedef struct _HIDP_PREPARSED_DATA {
    ULONG   Magic;
    ULONG   Length;
    USAGE   Usage;
    USAGE   Page;
    USHORT  Lengths[3];         /* bytes incl. id byte, per report type */
    UCHAR   UsesIds;
    UCHAR   Pad;
    USHORT  NumNodes;
    USHORT  NumFields;
    USHORT  NumUsages;
    USHORT  NumRepLens;
    ULONG   NodesOff, FieldsOff, UsagesOff, RepLensOff;
} HIDP_PREPARSED_DATA;

#define PD_NODES(p)     ((HP_NODE *)((PUCHAR)(p) + (p)->NodesOff))
#define PD_FIELDS(p)    ((HP_FIELD *)((PUCHAR)(p) + (p)->FieldsOff))
#define PD_USAGES(p)    ((HP_USAGE *)((PUCHAR)(p) + (p)->UsagesOff))
#define PD_REPLENS(p)   ((HP_REPLEN *)((PUCHAR)(p) + (p)->RepLensOff))

/* ------------------------------------------------------------------ */
/* growable arrays (parse time)                                        */
/* ------------------------------------------------------------------ */
typedef struct _GROW {
    PUCHAR  Data;
    ULONG   Count, Cap, Size;
    int     Pool;
} GROW;

static void GrowInit(GROW *g, ULONG elem, int pool)
{
    g->Data = NULL; g->Count = g->Cap = 0; g->Size = elem; g->Pool = pool;
}

static void GrowFree(GROW *g)
{
    if (g->Data) HpFree(g->Data);
    g->Data = NULL; g->Count = g->Cap = 0;
}

/* returns a pointer to a new zeroed element, or NULL */
static void *GrowAdd(GROW *g)
{
    if (g->Count == g->Cap) {
        ULONG ncap = g->Cap ? g->Cap * 2 : 16;
        PUCHAR nd = (PUCHAR)HpAlloc(ncap * g->Size, g->Pool);
        if (!nd) return NULL;
        if (g->Data) { HpCopy(nd, g->Data, g->Count * g->Size); HpFree(g->Data); }
        g->Data = nd; g->Cap = ncap;
    }
    HpZero(g->Data + g->Count * g->Size, g->Size);
    return g->Data + g->Count++ * g->Size;
}

#define GROW_AT(g, T, i)    (((T *)(g)->Data) + (i))

/* ------------------------------------------------------------------ */
/* bit access                                                          */
/* ------------------------------------------------------------------ */
static ULONG GetBits(const UCHAR *data, ULONG pos, ULONG n)
{
    ULONG v = 0, i;
    for (i = 0; i < n; i++) {
        ULONG b = pos + i;
        if (data[b >> 3] & (1u << (b & 7)))
            v |= 1UL << i;
    }
    return v;
}

static void PutBits(UCHAR *data, ULONG pos, ULONG n, ULONG v)
{
    ULONG i;
    for (i = 0; i < n; i++) {
        ULONG b = pos + i;
        if (v & (1UL << i)) data[b >> 3] |=  (UCHAR)(1u << (b & 7));
        else                data[b >> 3] &= (UCHAR)~(1u << (b & 7));
    }
}

/* ------------------------------------------------------------------ */
/* the parser                                                          */
/* ------------------------------------------------------------------ */
typedef struct _GLOBALS {
    USAGE   Page;
    LONG    LogMin, LogMax, PhysMin, PhysMax;
    ULONG   UnitsExp, Units;
    ULONG   Size, Count;
    UCHAR   ReportId;
    UCHAR   LogMaxSet;
} GLOBALS;

#define MAX_LOCAL_USAGES    512
#define MAX_PUSH            8

typedef struct _LOCALS {
    HP_USAGE    List[MAX_LOCAL_USAGES];
    ULONG       NumUsages;
    int         HasMin, HasMax;
    ULONG       UsageMin, UsageMax;          /* extended (page<<16|usage) */
    ULONG       DesMin, DesMax, StrMin, StrMax;
} LOCALS;

/* parse-time field: a HP_FIELD plus its parse-time owner */
typedef struct _PFIELD {
    HP_FIELD    F;
    USHORT      Node;           /* device-wide node index */
    USHORT      FirstUsage;     /* index into the usage list copy */
} PFIELD;

typedef struct _PNODE {
    HP_NODE     N;
    USHORT      Top;            /* device-wide index of its top-level node */
    USHORT      LastChild;
} PNODE;

typedef struct _REPCOUNT {
    UCHAR   Id;
    UCHAR   Type;
    ULONG   Bits;
} REPCOUNT;

typedef struct _PARSE {
    GROW    Nodes;      /* PNODE   */
    GROW    Fields;     /* PFIELD  */
    GROW    Usages;     /* HP_USAGE (array-field usage lists) */
    GROW    Reps;       /* REPCOUNT */
    int     Pool;
    int     UsesIds;
} PARSE;

static REPCOUNT *RepFor(PARSE *ps, UCHAR id, UCHAR type)
{
    ULONG i;
    REPCOUNT *r;
    for (i = 0; i < ps->Reps.Count; i++) {
        r = GROW_AT(&ps->Reps, REPCOUNT, i);
        if (r->Id == id && r->Type == type) return r;
    }
    r = (REPCOUNT *)GrowAdd(&ps->Reps);
    if (r) { r->Id = id; r->Type = type; r->Bits = 0; }
    return r;
}

static LONG SignExtend(ULONG v, int nbytes)
{
    if (nbytes == 1) return (LONG)(signed char)v;
    if (nbytes == 2) return (LONG)(short)v;
    return (LONG)v;
}

/* one main item (input/output/feature) -> fields */
static NTSTATUS AddMainItem(PARSE *ps, GLOBALS *g, LOCALS *l, UCHAR type,
                            ULONG flags, USHORT node)
{
    REPCOUNT *rc = RepFor(ps, g->ReportId, type);
    ULONG bitpos, i, n;
    PFIELD *pf;
    HP_USAGE *u;

    if (!rc) return STATUS_INSUFFICIENT_RESOURCES;
    if (g->ReportId) ps->UsesIds = 1;
    bitpos = rc->Bits;
    rc->Bits += g->Size * g->Count;

    if ((flags & MF_CONST) || g->Count == 0)
        return STATUS_SUCCESS;                       /* padding */

#define NEWFIELD(_pf_) \
    _pf_ = (PFIELD *)GrowAdd(&ps->Fields); \
    if (!_pf_) return STATUS_INSUFFICIENT_RESOURCES; \
    _pf_->Node = node; \
    _pf_->F.Type = type; _pf_->F.ReportId = g->ReportId; \
    _pf_->F.Flags = (USHORT)flags; _pf_->F.BitSize = (USHORT)g->Size; \
    _pf_->F.LogMin = g->LogMin; _pf_->F.LogMax = g->LogMax; \
    _pf_->F.PhysMin = g->PhysMin; _pf_->F.PhysMax = g->PhysMax; \
    _pf_->F.Units = g->Units; _pf_->F.UnitsExp = g->UnitsExp; \
    _pf_->F.StringMin = (USHORT)l->StrMin; _pf_->F.StringMax = (USHORT)l->StrMax; \
    _pf_->F.DesMin = (USHORT)l->DesMin; _pf_->F.DesMax = (USHORT)l->DesMax;

    if (!(flags & MF_VARIABLE)) {
        /* array item: one field, slots select among the usages */
        NEWFIELD(pf);
        pf->F.Kind = FK_ARRAY;
        pf->F.BitPos = bitpos;
        pf->F.Count = (USHORT)g->Count;
        if (l->HasMin && l->HasMax) {
            pf->F.IsRange = 1;
            pf->F.Page = (USAGE)((l->UsageMin >> 16) ? (l->UsageMin >> 16) : g->Page);
            pf->F.UsageMin = (USAGE)l->UsageMin;
            pf->F.UsageMax = (USAGE)l->UsageMax;
        } else if (l->NumUsages) {
            pf->F.Page = l->List[0].Page;
            pf->F.ListCount = (USHORT)l->NumUsages;
            pf->FirstUsage = (USHORT)ps->Usages.Count;
            for (i = 0; i < l->NumUsages; i++) {
                u = (HP_USAGE *)GrowAdd(&ps->Usages);
                if (!u) return STATUS_INSUFFICIENT_RESOURCES;
                *u = l->List[i];
            }
            pf->F.UsageMin = l->List[0].Usage;
            pf->F.UsageMax = l->List[l->NumUsages - 1].Usage;
        } else {
            return STATUS_SUCCESS;                   /* no usage: ignore */
        }
        return STATUS_SUCCESS;
    }

    /* variable item */
    if (l->HasMin && l->HasMax) {
        ULONG span = l->UsageMax - l->UsageMin + 1;
        USAGE page = (USAGE)((l->UsageMin >> 16) ? (l->UsageMin >> 16) : g->Page);

        n = span < g->Count ? span : g->Count;       /* slots covered by the range */
        NEWFIELD(pf);
        pf->F.Kind = (g->Size == 1) ? FK_BUTTON : FK_VALUE;
        pf->F.BitPos = bitpos;
        pf->F.Count = (USHORT)n;
        pf->F.IsRange = (n > 1);
        pf->F.Page = page;
        pf->F.UsageMin = (USAGE)l->UsageMin;
        pf->F.UsageMax = (USAGE)(l->UsageMin + n - 1);
        for (i = n; i < g->Count; i++) {             /* excess slots: last usage */
            NEWFIELD(pf);
            pf->F.Kind = (g->Size == 1) ? FK_BUTTON : FK_VALUE;
            pf->F.BitPos = bitpos + i * g->Size;
            pf->F.Count = 1;
            pf->F.Page = page;
            pf->F.UsageMin = pf->F.UsageMax = (USAGE)l->UsageMax;
        }
        return STATUS_SUCCESS;
    }
    if (!l->NumUsages)
        return STATUS_SUCCESS;                       /* no usage: ignore */

    if (l->NumUsages == 1 && g->Size > 1 && g->Count > 1) {
        /* one usage over several wide slots: a value array */
        NEWFIELD(pf);
        pf->F.Kind = FK_VALUE;
        pf->F.BitPos = bitpos;
        pf->F.Count = (USHORT)g->Count;
        pf->F.Page = l->List[0].Page;
        pf->F.UsageMin = pf->F.UsageMax = l->List[0].Usage;
        return STATUS_SUCCESS;
    }
    for (i = 0; i < g->Count; i++) {                 /* one field per slot */
        u = &l->List[i < l->NumUsages ? i : l->NumUsages - 1];
        NEWFIELD(pf);
        pf->F.Kind = (g->Size == 1) ? FK_BUTTON : FK_VALUE;
        pf->F.BitPos = bitpos + i * g->Size;
        pf->F.Count = 1;
        pf->F.Page = u->Page;
        pf->F.UsageMin = pf->F.UsageMax = u->Usage;
    }
    return STATUS_SUCCESS;
#undef NEWFIELD
}

static void ResetLocals(LOCALS *l)
{
    l->NumUsages = 0;
    l->HasMin = l->HasMax = 0;
    l->UsageMin = l->UsageMax = 0;
    l->DesMin = l->DesMax = l->StrMin = l->StrMax = 0;
}

static NTSTATUS ParseDescriptor(PARSE *ps, const UCHAR *d, ULONG len)
{
    GLOBALS g, stack[MAX_PUSH];
    LOCALS *l;
    USHORT open[32];                 /* open collection stack (device-wide) */
    int depth = 0, sp = 0;
    ULONG pos = 0;
    NTSTATUS st = STATUS_SUCCESS;

    l = (LOCALS *)HpAlloc(sizeof(LOCALS), ps->Pool);
    if (!l) return STATUS_INSUFFICIENT_RESOURCES;
    HpZero(&g, sizeof(g));
    ResetLocals(l);

    while (pos < len) {
        UCHAR prefix = d[pos++];
        ULONG size, tag, type, val = 0, i;
        LONG sval;

        if (prefix == 0xFE) {                        /* long item: skip */
            if (pos + 2 > len) { st = STATUS_BUFFER_TOO_SMALL; break; }
            pos += 2 + d[pos];
            continue;
        }
        size = prefix & 3; if (size == 3) size = 4;
        type = (prefix >> 2) & 3;
        tag  = prefix >> 4;
        if (pos + size > len) { st = STATUS_BUFFER_TOO_SMALL; break; }
        for (i = 0; i < size; i++) val |= (ULONG)d[pos + i] << (8 * i);
        pos += size;
        sval = SignExtend(val, (int)size);

        if (type == 0) {                             /* main */
            if (tag == 8 || tag == 9 || tag == 0xB) {
                if (depth == 0) { st = STATUS_INVALID_PARAMETER_MIX; break; }
                st = AddMainItem(ps, &g, l,
                                 (UCHAR)(tag == 8 ? HidP_Input : tag == 9 ? HidP_Output : HidP_Feature),
                                 val, open[depth - 1]);
                if (!NT_SUCCESS(st)) break;
            } else if (tag == 0xA) {                 /* collection */
                PNODE *pn;
                USHORT idx;
                if (depth >= 32) { st = STATUS_INVALID_PARAMETER; break; }
                idx = (USHORT)ps->Nodes.Count;
                pn = (PNODE *)GrowAdd(&ps->Nodes);
                if (!pn) { st = STATUS_INSUFFICIENT_RESOURCES; break; }
                pn->N.Type = (UCHAR)val;
                if (l->NumUsages) {
                    pn->N.Page = l->List[0].Page; pn->N.Usage = l->List[0].Usage;
                } else {
                    pn->N.Page = g.Page; pn->N.Usage = 0;
                }
                pn->N.FirstChild = pn->N.NextSibling = HP_NONE;
                pn->LastChild = HP_NONE;
                if (depth == 0) {
                    pn->N.Parent = HP_NONE; pn->Top = idx;
                } else {
                    PNODE *par = GROW_AT(&ps->Nodes, PNODE, open[depth - 1]);
                    pn->N.Parent = open[depth - 1];
                    pn->Top = par->Top;
                    par->N.NumChildren++;
                    if (par->N.FirstChild == HP_NONE) par->N.FirstChild = idx;
                    else GROW_AT(&ps->Nodes, PNODE, par->LastChild)->N.NextSibling = idx;
                    par->LastChild = idx;
                }
                open[depth++] = idx;
            } else if (tag == 0xC) {                 /* end collection */
                if (depth == 0) { st = STATUS_INVALID_PARAMETER; break; }
                depth--;
            }
            ResetLocals(l);
        } else if (type == 1) {                      /* global */
            switch (tag) {
            case 0: g.Page = (USAGE)val; break;
            case 1: g.LogMin = sval; g.LogMaxSet = 0; break;
            case 2:
                g.LogMax = sval;
                if (g.LogMax < g.LogMin && size < 4)  /* unsigned maximum */
                    g.LogMax = (LONG)(val & (size == 1 ? 0xFFUL : 0xFFFFUL));
                break;
            case 3: g.PhysMin = sval; break;
            case 4: g.PhysMax = sval; break;
            case 5: g.UnitsExp = val; break;
            case 6: g.Units = val; break;
            case 7: g.Size = val; break;
            case 8: g.ReportId = (UCHAR)val; break;
            case 9: g.Count = val; break;
            case 0xA:
                if (sp >= MAX_PUSH) { st = STATUS_INVALID_PARAMETER; }
                else stack[sp++] = g;
                break;
            case 0xB:
                if (sp == 0) { st = STATUS_INVALID_PARAMETER; }
                else g = stack[--sp];
                break;
            }
            if (!NT_SUCCESS(st)) break;
        } else if (type == 2) {                      /* local */
            switch (tag) {
            case 0:
                if (l->NumUsages < MAX_LOCAL_USAGES) {
                    l->List[l->NumUsages].Page  = (USAGE)(size == 4 ? (val >> 16) : g.Page);
                    l->List[l->NumUsages].Usage = (USAGE)val;
                    l->NumUsages++;
                }
                break;
            case 1: l->UsageMin = val; l->HasMin = 1; break;
            case 2: l->UsageMax = val; l->HasMax = 1; break;
            case 4: l->DesMin = val; break;
            case 5: l->DesMax = val; break;
            case 8: l->StrMin = val; break;
            case 9: l->StrMax = val; break;
            }
        }
    }
    if (NT_SUCCESS(st) && depth != 0)
        st = STATUS_COULD_NOT_INTERPRET;
    HpFree(l);
    return st;
}

/* ------------------------------------------------------------------ */
/* building the preparsed data of one top-level collection             */
/* ------------------------------------------------------------------ */
static ULONG CountIndices(const HP_FIELD *f)
{
    if (f->Kind == FK_ARRAY)
        return f->IsRange ? (ULONG)(f->UsageMax - f->UsageMin + 1)
                          : (f->ListCount ? f->ListCount : 1);
    if (f->Kind == FK_VALUE)
        return 1;
    return f->IsRange ? (ULONG)(f->UsageMax - f->UsageMin + 1) : 1;
}

static PHIDP_PREPARSED_DATA BuildPreparsed(PARSE *ps, USHORT top, int pool)
{
    ULONG i, j, nn = 0, nf = 0, nu = 0, nr = 0, total;
    USHORT *map;
    PHIDP_PREPARSED_DATA pd;
    HP_NODE *outn;
    HP_FIELD *outf;
    HP_USAGE *outu;
    HP_REPLEN *outr;
    USHORT dataidx[3];
    PNODE *tn;

    map = (USHORT *)HpAlloc(ps->Nodes.Count * sizeof(USHORT) + 2, pool);
    if (!map) return NULL;
    for (i = 0; i < ps->Nodes.Count; i++) {
        if (GROW_AT(&ps->Nodes, PNODE, i)->Top == top) map[i] = (USHORT)nn++;
        else map[i] = HP_NONE;
    }
    for (i = 0; i < ps->Fields.Count; i++) {
        PFIELD *pf = GROW_AT(&ps->Fields, PFIELD, i);
        if (map[pf->Node] != HP_NONE) {
            nf++;
            if (pf->F.Kind == FK_ARRAY && pf->F.ListCount) nu += pf->F.ListCount;
        }
    }
    /* report lengths used by this collection */
    for (i = 0; i < ps->Reps.Count; i++) {
        REPCOUNT *r = GROW_AT(&ps->Reps, REPCOUNT, i);
        int used = 0;
        for (j = 0; j < ps->Fields.Count; j++) {
            PFIELD *pf = GROW_AT(&ps->Fields, PFIELD, j);
            if (map[pf->Node] != HP_NONE && pf->F.ReportId == r->Id && pf->F.Type == r->Type)
                { used = 1; break; }
        }
        if (used) nr++;
    }

    total = sizeof(HIDP_PREPARSED_DATA) + nn * sizeof(HP_NODE) + nf * sizeof(HP_FIELD)
          + nu * sizeof(HP_USAGE) + nr * sizeof(HP_REPLEN);
    pd = (PHIDP_PREPARSED_DATA)HpAlloc(total, pool);
    if (!pd) { HpFree(map); return NULL; }
    HpZero(pd, total);

    pd->Magic = HP_MAGIC;
    pd->Length = total;
    tn = GROW_AT(&ps->Nodes, PNODE, top);
    pd->Usage = tn->N.Usage;
    pd->Page = tn->N.Page;
    pd->UsesIds = (UCHAR)ps->UsesIds;
    pd->NumNodes = (USHORT)nn; pd->NumFields = (USHORT)nf;
    pd->NumUsages = (USHORT)nu; pd->NumRepLens = (USHORT)nr;
    pd->NodesOff   = sizeof(HIDP_PREPARSED_DATA);
    pd->FieldsOff  = pd->NodesOff + nn * sizeof(HP_NODE);
    pd->UsagesOff  = pd->FieldsOff + nf * sizeof(HP_FIELD);
    pd->RepLensOff = pd->UsagesOff + nu * sizeof(HP_USAGE);
    outn = PD_NODES(pd); outf = PD_FIELDS(pd); outu = PD_USAGES(pd); outr = PD_REPLENS(pd);

    for (i = 0; i < ps->Nodes.Count; i++) {
        PNODE *p = GROW_AT(&ps->Nodes, PNODE, i);
        HP_NODE *o;
        if (map[i] == HP_NONE) continue;
        o = &outn[map[i]];
        *o = p->N;
        o->Parent      = (p->N.Parent == HP_NONE)      ? 0 : map[p->N.Parent];
        o->NextSibling = (p->N.NextSibling == HP_NONE) ? 0 : map[p->N.NextSibling];
        o->FirstChild  = (p->N.FirstChild == HP_NONE)  ? 0 : map[p->N.FirstChild];
    }

    dataidx[0] = dataidx[1] = dataidx[2] = 0;
    nf = 0; nu = 0;
    for (i = 0; i < ps->Fields.Count; i++) {
        PFIELD *pf = GROW_AT(&ps->Fields, PFIELD, i);
        HP_FIELD *o;
        ULONG k;
        if (map[pf->Node] == HP_NONE) continue;
        o = &outf[nf++];
        *o = pf->F;
        o->Node = map[pf->Node];
        if (pf->F.Kind == FK_ARRAY && pf->F.ListCount) {
            o->ListOffset = nu;
            for (k = 0; k < pf->F.ListCount; k++)
                outu[nu++] = *GROW_AT(&ps->Usages, HP_USAGE, pf->FirstUsage + k);
        }
        o->DataIndexMin = dataidx[o->Type];
        dataidx[o->Type] = (USHORT)(dataidx[o->Type] + CountIndices(o));
        o->DataIndexMax = (USHORT)(dataidx[o->Type] - 1);
    }

    nr = 0;
    for (i = 0; i < ps->Reps.Count; i++) {
        REPCOUNT *r = GROW_AT(&ps->Reps, REPCOUNT, i);
        int used = 0;
        for (j = 0; j < ps->Fields.Count; j++) {
            PFIELD *pf = GROW_AT(&ps->Fields, PFIELD, j);
            if (map[pf->Node] != HP_NONE && pf->F.ReportId == r->Id && pf->F.Type == r->Type)
                { used = 1; break; }
        }
        if (!used) continue;
        outr[nr].Id = r->Id; outr[nr].Type = r->Type;
        outr[nr].Bytes = (USHORT)((r->Bits + 7) / 8);
        nr++;
        {
            /* the report id byte is always counted (0 when the device has no ids) */
            USHORT bytes = (USHORT)(((r->Bits + 7) / 8) + 1);
            if (bytes > pd->Lengths[r->Type]) pd->Lengths[r->Type] = bytes;
        }
    }
    HpFree(map);
    return pd;
}

NTSTATUS HidP_GetCollectionDescription(
    IN PHIDP_REPORT_DESCRIPTOR ReportDesc, IN ULONG DescLength,
    IN POOL_TYPE PoolType, OUT PHIDP_DEVICE_DESC Dev)
{
    PARSE ps;
    NTSTATUS st;
    ULONG i, ncol = 0, nid = 0, c;
    PHIDP_COLLECTION_DESC cd = NULL;
    PHIDP_REPORT_IDS ids = NULL;

    HpZero(Dev, sizeof(*Dev));
    HpZero(&ps, sizeof(ps));
    ps.Pool = (int)PoolType;
    GrowInit(&ps.Nodes, sizeof(PNODE), ps.Pool);
    GrowInit(&ps.Fields, sizeof(PFIELD), ps.Pool);
    GrowInit(&ps.Usages, sizeof(HP_USAGE), ps.Pool);
    GrowInit(&ps.Reps, sizeof(REPCOUNT), ps.Pool);

    st = ParseDescriptor(&ps, ReportDesc, DescLength);
    if (!NT_SUCCESS(st)) goto done;

    for (i = 0; i < ps.Nodes.Count; i++)
        if (GROW_AT(&ps.Nodes, PNODE, i)->Top == i) ncol++;
    if (!ncol) { st = STATUS_COULD_NOT_INTERPRET; goto done; }

    cd = (PHIDP_COLLECTION_DESC)HpAlloc(ncol * sizeof(HIDP_COLLECTION_DESC), ps.Pool);
    if (!cd) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    HpZero(cd, ncol * sizeof(HIDP_COLLECTION_DESC));

    for (i = 0, c = 0; i < ps.Nodes.Count; i++) {
        PNODE *pn = GROW_AT(&ps.Nodes, PNODE, i);
        PHIDP_PREPARSED_DATA pd;
        if (pn->Top != i) continue;
        pd = BuildPreparsed(&ps, (USHORT)i, ps.Pool);
        if (!pd) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
        cd[c].UsagePage = pd->Page;
        cd[c].Usage = pd->Usage;
        cd[c].CollectionNumber = (UCHAR)(c + 1);
        cd[c].InputLength = pd->Lengths[HidP_Input];
        cd[c].OutputLength = pd->Lengths[HidP_Output];
        cd[c].FeatureLength = pd->Lengths[HidP_Feature];
        cd[c].PreparsedDataLength = (USHORT)pd->Length;
        cd[c].PreparsedData = pd;
        c++;
    }

    /* report ids (one entry per id, the collection that owns it) */
    {
        UCHAR seen[256];
        HpZero(seen, sizeof(seen));
        for (i = 0; i < ps.Fields.Count; i++) {
            PFIELD *pf = GROW_AT(&ps.Fields, PFIELD, i);
            if (!seen[pf->F.ReportId]) { seen[pf->F.ReportId] = 1; nid++; }
        }
        if (nid) {
            ids = (PHIDP_REPORT_IDS)HpAlloc(nid * sizeof(HIDP_REPORT_IDS), ps.Pool);
            if (!ids) { st = STATUS_INSUFFICIENT_RESOURCES; goto done; }
            HpZero(ids, nid * sizeof(HIDP_REPORT_IDS));
            HpZero(seen, sizeof(seen));
            for (i = 0, c = 0; i < ps.Fields.Count; i++) {
                PFIELD *pf = GROW_AT(&ps.Fields, PFIELD, i);
                ULONG k, col = 0;
                if (seen[pf->F.ReportId]) continue;
                seen[pf->F.ReportId] = 1;
                ids[c].ReportID = pf->F.ReportId;
                {
                    USHORT top = GROW_AT(&ps.Nodes, PNODE, pf->Node)->Top;
                    for (k = 0; k < ps.Nodes.Count; k++) {
                        if (GROW_AT(&ps.Nodes, PNODE, k)->Top == k) {
                            col++;
                            if (k == top) break;
                        }
                    }
                }
                ids[c].CollectionNumber = (UCHAR)col;
                for (k = 0; k < ps.Reps.Count; k++) {
                    REPCOUNT *r = GROW_AT(&ps.Reps, REPCOUNT, k);
                    USHORT bytes;
                    if (r->Id != pf->F.ReportId) continue;
                    bytes = (USHORT)(((r->Bits + 7) / 8) + 1);      /* + the id byte, always */
                    if (r->Type == HidP_Input)  ids[c].InputLength = bytes;
                    if (r->Type == HidP_Output) ids[c].OutputLength = bytes;
                    if (r->Type == HidP_Feature) ids[c].FeatureLength = bytes;
                }
                c++;
            }
        }
    }

    Dev->CollectionDesc = cd;       cd = NULL;
    Dev->CollectionDescLength = ncol;
    Dev->ReportIDs = ids;           ids = NULL;
    Dev->ReportIDsLength = nid;

done:
    if (cd) {
        for (i = 0; i < ncol; i++)
            if (cd[i].PreparsedData) HpFree(cd[i].PreparsedData);
        HpFree(cd);
    }
    if (ids) HpFree(ids);
    GrowFree(&ps.Nodes); GrowFree(&ps.Fields); GrowFree(&ps.Usages); GrowFree(&ps.Reps);
    return st;
}

VOID HidP_FreeCollectionDescription(IN PHIDP_DEVICE_DESC Dev)
{
    ULONG i;
    if (Dev->CollectionDesc) {
        for (i = 0; i < Dev->CollectionDescLength; i++)
            if (Dev->CollectionDesc[i].PreparsedData)
                HpFree(Dev->CollectionDesc[i].PreparsedData);
        HpFree(Dev->CollectionDesc);
    }
    if (Dev->ReportIDs) HpFree(Dev->ReportIDs);
    HpZero(Dev, sizeof(*Dev));
}

/* ------------------------------------------------------------------ */
/* report access API                                                   */
/* ------------------------------------------------------------------ */
static int PdValid(PHIDP_PREPARSED_DATA pd)
{
    return pd && pd->Magic == HP_MAGIC;
}

static int IsButtonField(const HP_FIELD *f)
{
    return f->Kind == FK_BUTTON || f->Kind == FK_ARRAY;
}

NTSTATUS __stdcall HidP_GetCaps(IN PHIDP_PREPARSED_DATA pd, OUT PHIDP_CAPS caps)
{
    ULONG i;
    HP_FIELD *f;

    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    HpZero(caps, sizeof(*caps));
    caps->Usage = pd->Usage;
    caps->UsagePage = pd->Page;
    caps->InputReportByteLength = pd->Lengths[HidP_Input];
    caps->OutputReportByteLength = pd->Lengths[HidP_Output];
    caps->FeatureReportByteLength = pd->Lengths[HidP_Feature];
    caps->NumberLinkCollectionNodes = pd->NumNodes;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        ULONG ni = CountIndices(&f[i]);
        int btn = IsButtonField(&f[i]);
        switch (f[i].Type) {
        case HidP_Input:
            if (btn) caps->NumberInputButtonCaps++; else caps->NumberInputValueCaps++;
            caps->NumberInputDataIndices = (USHORT)(caps->NumberInputDataIndices + ni); break;
        case HidP_Output:
            if (btn) caps->NumberOutputButtonCaps++; else caps->NumberOutputValueCaps++;
            caps->NumberOutputDataIndices = (USHORT)(caps->NumberOutputDataIndices + ni); break;
        case HidP_Feature:
            if (btn) caps->NumberFeatureButtonCaps++; else caps->NumberFeatureValueCaps++;
            caps->NumberFeatureDataIndices = (USHORT)(caps->NumberFeatureDataIndices + ni); break;
        }
    }
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS __stdcall HidP_GetLinkCollectionNodes(
    OUT PHIDP_LINK_COLLECTION_NODE out, IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd)
{
    ULONG i;
    HP_NODE *n;

    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if (*len < pd->NumNodes) { *len = pd->NumNodes; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    n = PD_NODES(pd);
    for (i = 0; i < pd->NumNodes; i++) {
        HpZero(&out[i], sizeof(out[i]));
        out[i].LinkUsage = n[i].Usage;
        out[i].LinkUsagePage = n[i].Page;
        out[i].Parent = n[i].Parent;
        out[i].NumberOfChildren = n[i].NumChildren;
        out[i].NextSibling = n[i].NextSibling;
        out[i].FirstChild = n[i].FirstChild;
        out[i].CollectionType = n[i].Type;
    }
    *len = pd->NumNodes;
    return HIDP_STATUS_SUCCESS;
}

static int Matches(const HP_FIELD *f, USAGE page, USHORT link, USAGE usage)
{
    if (page && f->Page != page) return 0;
    if (link && f->Node != link) return 0;
    if (usage) {
        if (f->Kind == FK_ARRAY && f->ListCount) return 1;
        if (usage < f->UsageMin || usage > f->UsageMax) return 0;
    }
    return 1;
}

NTSTATUS __stdcall HidP_GetSpecificButtonCaps(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    OUT PHIDP_BUTTON_CAPS caps, IN OUT PUSHORT len, IN PHIDP_PREPARSED_DATA pd)
{
    ULONG i, n = 0;
    HP_FIELD *f;
    HP_NODE *nodes;

    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if ((int)type < 0 || (int)type > 2) return HIDP_STATUS_INVALID_REPORT_TYPE;
    f = PD_FIELDS(pd); nodes = PD_NODES(pd);
    for (i = 0; i < pd->NumFields; i++) {
        PHIDP_BUTTON_CAPS c;
        if (f[i].Type != (UCHAR)type || !IsButtonField(&f[i]) ||
            !Matches(&f[i], page, link, usage)) continue;
        if (n < *len) {
            c = &caps[n];
            HpZero(c, sizeof(*c));
            c->UsagePage = f[i].Page;
            c->ReportID = f[i].ReportId;
            c->BitField = f[i].Flags;
            c->LinkCollection = f[i].Node;
            c->LinkUsage = nodes[f[i].Node].Usage;
            c->LinkUsagePage = nodes[f[i].Node].Page;
            c->IsRange = (BOOLEAN)(f[i].IsRange || (f[i].Kind == FK_ARRAY && f[i].ListCount > 1));
            c->IsAbsolute = (BOOLEAN)!(f[i].Flags & MF_RELATIVE);
            if (c->IsRange) {
                c->Range.UsageMin = f[i].UsageMin; c->Range.UsageMax = f[i].UsageMax;
                c->Range.DataIndexMin = f[i].DataIndexMin; c->Range.DataIndexMax = f[i].DataIndexMax;
            } else {
                c->NotRange.Usage = f[i].UsageMin;
                c->NotRange.DataIndex = f[i].DataIndexMin;
            }
        }
        n++;
    }
    if (n > *len) { *len = (USHORT)n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *len = (USHORT)n;
    return n ? HIDP_STATUS_SUCCESS : HIDP_STATUS_USAGE_NOT_FOUND;
}

NTSTATUS __stdcall HidP_GetSpecificValueCaps(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    OUT PHIDP_VALUE_CAPS caps, IN OUT PUSHORT len, IN PHIDP_PREPARSED_DATA pd)
{
    ULONG i, n = 0;
    HP_FIELD *f;
    HP_NODE *nodes;

    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if ((int)type < 0 || (int)type > 2) return HIDP_STATUS_INVALID_REPORT_TYPE;
    f = PD_FIELDS(pd); nodes = PD_NODES(pd);
    for (i = 0; i < pd->NumFields; i++) {
        PHIDP_VALUE_CAPS c;
        if (f[i].Type != (UCHAR)type || IsButtonField(&f[i]) ||
            !Matches(&f[i], page, link, usage)) continue;
        if (n < *len) {
            c = &caps[n];
            HpZero(c, sizeof(*c));
            c->UsagePage = f[i].Page;
            c->ReportID = f[i].ReportId;
            c->BitField = f[i].Flags;
            c->LinkCollection = f[i].Node;
            c->LinkUsage = nodes[f[i].Node].Usage;
            c->LinkUsagePage = nodes[f[i].Node].Page;
            c->IsRange = f[i].IsRange;
            c->IsAbsolute = (BOOLEAN)!(f[i].Flags & MF_RELATIVE);
            c->HasNull = (BOOLEAN)((f[i].Flags & MF_NULLSTATE) != 0);
            c->BitSize = f[i].BitSize;
            c->ReportCount = f[i].Count;
            c->UnitsExp = f[i].UnitsExp; c->Units = f[i].Units;
            c->LogicalMin = f[i].LogMin; c->LogicalMax = f[i].LogMax;
            c->PhysicalMin = f[i].PhysMin; c->PhysicalMax = f[i].PhysMax;
            if (c->IsRange) {
                c->Range.UsageMin = f[i].UsageMin; c->Range.UsageMax = f[i].UsageMax;
                c->Range.DataIndexMin = f[i].DataIndexMin; c->Range.DataIndexMax = f[i].DataIndexMax;
            } else {
                c->NotRange.Usage = f[i].UsageMin;
                c->NotRange.DataIndex = f[i].DataIndexMin;
            }
        }
        n++;
    }
    if (n > *len) { *len = (USHORT)n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *len = (USHORT)n;
    return n ? HIDP_STATUS_SUCCESS : HIDP_STATUS_USAGE_NOT_FOUND;
}

/* Validate a report buffer; returns the data pointer (after the id byte) and
 * the id, or an error status. */
static NTSTATUS CheckReport(PHIDP_PREPARSED_DATA pd, HIDP_REPORT_TYPE type,
                            PCHAR report, ULONG rlen, PUCHAR *data, UCHAR *id)
{
    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if ((int)type < 0 || (int)type > 2) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (rlen != pd->Lengths[type]) return HIDP_STATUS_INVALID_REPORT_LENGTH;
    /* as on NT: byte 0 is the report id, also for a device without ids (then 0) */
    *id = (UCHAR)report[0];
    *data = (PUCHAR)report + 1;
    return HIDP_STATUS_SUCCESS;
}

static USAGE ArrayUsage(const HP_FIELD *f, PHIDP_PREPARSED_DATA pd, LONG v)
{
    LONG idx = v - f->LogMin;
    if (idx < 0) return 0;
    if (f->ListCount) {
        if ((ULONG)idx >= f->ListCount) return 0;
        return PD_USAGES(pd)[f->ListOffset + idx].Usage;
    }
    if ((ULONG)idx > (ULONG)(f->UsageMax - f->UsageMin)) return 0;
    return (USAGE)(f->UsageMin + idx);
}

NTSTATUS __stdcall HidP_GetUsages(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, OUT USAGE *list,
    IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    ULONG i, k, n = 0;
    int found = 0;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || f[i].ReportId != id || !IsButtonField(&f[i])) continue;
        if (page && f[i].Page != page) continue;
        if (link && f[i].Node != link) continue;
        found = 1;
        if (f[i].Kind == FK_BUTTON) {
            for (k = 0; k < f[i].Count; k++) {
                if (GetBits(data, f[i].BitPos + k * f[i].BitSize, 1)) {
                    if (n < *len) list[n] = (USAGE)(f[i].UsageMin + k);
                    n++;
                }
            }
        } else {
            for (k = 0; k < f[i].Count; k++) {
                LONG v = (LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize);
                USAGE u = ArrayUsage(&f[i], pd, v);
                if (u) {
                    if (n < *len) list[n] = u;
                    n++;
                }
            }
        }
    }
    if (!found) { *len = 0; return HIDP_STATUS_USAGE_NOT_FOUND; }
    if (n > *len) { *len = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *len = n;
    return HIDP_STATUS_SUCCESS;
}

ULONG __stdcall HidP_MaxUsageListLength(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN PHIDP_PREPARSED_DATA pd)
{
    ULONG i, n = 0;
    HP_FIELD *f;

    if (!PdValid(pd) || (int)type < 0 || (int)type > 2) return 0;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || !IsButtonField(&f[i])) continue;
        if (page && f[i].Page != page) continue;
        n += (f[i].Kind == FK_ARRAY) ? f[i].Count : f[i].Count;
    }
    return n;
}

/* find the slot for a usage in a button field; returns the bit position or -1 */
static NTSTATUS SetUnsetUsage(PHIDP_PREPARSED_DATA pd, HIDP_REPORT_TYPE type,
                              USAGE page, USHORT link, USAGE usage, UCHAR id,
                              PUCHAR data, int set)
{
    ULONG i, k;
    HP_FIELD *f = PD_FIELDS(pd);
    int found = 0;

    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || f[i].ReportId != id || !IsButtonField(&f[i])) continue;
        if (f[i].Page != page || (link && f[i].Node != link)) continue;
        if (f[i].Kind == FK_BUTTON) {
            if (usage < f[i].UsageMin || usage > f[i].UsageMax) continue;
            PutBits(data, f[i].BitPos + (ULONG)(usage - f[i].UsageMin) * f[i].BitSize,
                    1, set ? 1 : 0);
            return HIDP_STATUS_SUCCESS;
        } else {
            LONG want = -1;
            if (f[i].ListCount) {
                for (k = 0; k < f[i].ListCount; k++)
                    if (PD_USAGES(pd)[f[i].ListOffset + k].Usage == usage) { want = (LONG)k; break; }
            } else if (usage >= f[i].UsageMin && usage <= f[i].UsageMax) {
                want = (LONG)(usage - f[i].UsageMin);
            }
            if (want < 0) continue;
            want += f[i].LogMin;
            found = 1;
            if (set) {
                for (k = 0; k < f[i].Count; k++)         /* already present? */
                    if ((LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize) == want)
                        return HIDP_STATUS_SUCCESS;
                for (k = 0; k < f[i].Count; k++)         /* first empty slot */
                    if (GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize) == 0) {
                        PutBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize, (ULONG)want);
                        return HIDP_STATUS_SUCCESS;
                    }
                return HIDP_STATUS_BUFFER_TOO_SMALL;
            } else {
                for (k = 0; k < f[i].Count; k++)
                    if ((LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize) == want) {
                        PutBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize, 0);
                        return HIDP_STATUS_SUCCESS;
                    }
                return HIDP_STATUS_BUTTON_NOT_PRESSED;
            }
        }
    }
    return found ? HIDP_STATUS_BUTTON_NOT_PRESSED : HIDP_STATUS_USAGE_NOT_FOUND;
}

static NTSTATUS SetUnset(IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link,
    IN PUSAGE list, IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd,
    IN OUT PCHAR report, IN ULONG rlen, int set)
{
    PUCHAR data; UCHAR id;
    ULONG i;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    for (i = 0; i < *len; i++) {
        st = SetUnsetUsage(pd, type, page, link, list[i], id, data, set);
        if (!NT_SUCCESS(st)) { *len = i; return st; }
    }
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS __stdcall HidP_SetUsages(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN PUSAGE list,
    IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd, IN OUT PCHAR report, IN ULONG rlen)
{
    return SetUnset(type, page, link, list, len, pd, report, rlen, 1);
}

NTSTATUS __stdcall HidP_UnsetUsages(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN PUSAGE list,
    IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd, IN OUT PCHAR report, IN ULONG rlen)
{
    return SetUnset(type, page, link, list, len, pd, report, rlen, 0);
}

static HP_FIELD *FindValueField(PHIDP_PREPARSED_DATA pd, HIDP_REPORT_TYPE type,
                                USAGE page, USHORT link, USAGE usage, UCHAR id,
                                NTSTATUS *err)
{
    ULONG i;
    HP_FIELD *f = PD_FIELDS(pd);
    int found = 0;

    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || IsButtonField(&f[i])) continue;
        if (f[i].Page != page || (link && f[i].Node != link)) continue;
        if (usage < f[i].UsageMin || usage > f[i].UsageMax) continue;
        found = 1;
        if (f[i].ReportId != id) continue;
        return &f[i];
    }
    *err = found ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
    return NULL;
}

NTSTATUS __stdcall HidP_GetUsageValue(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    OUT PULONG value, IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    *value = GetBits(data, f->BitPos + (ULONG)(usage - f->UsageMin) * f->BitSize, f->BitSize);
    return HIDP_STATUS_SUCCESS;
}

/* logical -> physical value, integer only (no floating point in the kernel).
 * Exact while (logical range * physical range) < 2^32. */
static LONG Rescale(LONG v, LONG lmin, LONG lmax, LONG pmin, LONG pmax)
{
    ULONG num = (ULONG)(v - lmin), lr = (ULONG)(lmax - lmin);
    ULONG pr, mag;
    int neg = 0;

    if (pmax >= pmin) pr = (ULONG)(pmax - pmin);
    else { pr = (ULONG)(pmin - pmax); neg = 1; }
    mag = (num / lr) * pr + ((num % lr) * pr) / lr;
    return neg ? pmin - (LONG)mag : pmin + (LONG)mag;
}

NTSTATUS __stdcall HidP_GetScaledUsageValue(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    OUT PLONG value, IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    ULONG raw;
    LONG v;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    raw = GetBits(data, f->BitPos + (ULONG)(usage - f->UsageMin) * f->BitSize, f->BitSize);
    v = (LONG)raw;
    if (f->LogMin < 0 && f->BitSize < 32 && (raw & (1UL << (f->BitSize - 1))))
        v = (LONG)(raw | (0xFFFFFFFFUL << f->BitSize));          /* sign extend */
    if ((f->Flags & MF_NULLSTATE) && (v < f->LogMin || v > f->LogMax)) {
        return HIDP_STATUS_NULL;
    }
    if (f->PhysMin == 0 && f->PhysMax == 0) {
        *value = v;
    } else {
        if (f->LogMax == f->LogMin) return HIDP_STATUS_BAD_LOG_PHY_VALUES;
        *value = Rescale(v, f->LogMin, f->LogMax, f->PhysMin, f->PhysMax);
    }
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS __stdcall HidP_SetUsageValue(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    IN ULONG value, IN PHIDP_PREPARSED_DATA pd, IN OUT PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    PutBits(data, f->BitPos + (ULONG)(usage - f->UsageMin) * f->BitSize, f->BitSize, value);
    return HIDP_STATUS_SUCCESS;
}

ULONG __stdcall HidP_MaxDataListLength(IN HIDP_REPORT_TYPE type, IN PHIDP_PREPARSED_DATA pd)
{
    ULONG i, n = 0;
    HP_FIELD *f;

    if (!PdValid(pd) || (int)type < 0 || (int)type > 2) return 0;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type) continue;
        if (f[i].Kind == FK_ARRAY) n += f[i].Count;       /* one entry per slot */
        else if (f[i].Kind == FK_BUTTON) n += f[i].Count;
        else n += 1;
    }
    return n;
}

NTSTATUS __stdcall HidP_GetData(
    IN HIDP_REPORT_TYPE type, OUT PHIDP_DATA list, IN OUT PULONG len,
    IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    ULONG i, k, n = 0;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

#define EMIT(_idx_, _val_, _on_) \
    do { if (n < *len) { list[n].DataIndex = (USHORT)(_idx_); list[n].Reserved = 0; \
         if (_on_) list[n].On = TRUE; else list[n].RawValue = (_val_); } n++; } while (0)

    if (!NT_SUCCESS(st)) return st;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || f[i].ReportId != id) continue;
        if (f[i].Kind == FK_BUTTON) {
            for (k = 0; k < f[i].Count; k++)
                if (GetBits(data, f[i].BitPos + k * f[i].BitSize, 1))
                    EMIT(f[i].DataIndexMin + k, 0, 1);
        } else if (f[i].Kind == FK_ARRAY) {
            for (k = 0; k < f[i].Count; k++) {
                LONG v = (LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize);
                if (ArrayUsage(&f[i], pd, v))
                    EMIT(f[i].DataIndexMin + (v - f[i].LogMin), 0, 1);
            }
        } else {
            if (f[i].Count == 1)
                EMIT(f[i].DataIndexMin, GetBits(data, f[i].BitPos, f[i].BitSize), 0);
        }
    }
    if (n > *len) { *len = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *len = n;
    return HIDP_STATUS_SUCCESS;
#undef EMIT
}

NTSTATUS __stdcall HidP_InitializeReportForID(
    IN HIDP_REPORT_TYPE type, IN UCHAR id, IN PHIDP_PREPARSED_DATA pd,
    IN OUT PCHAR report, IN ULONG rlen)
{
    ULONG i;
    int exists = 0;
    HP_FIELD *f;

    if (!PdValid(pd)) return HIDP_STATUS_INVALID_PREPARSED_DATA;
    if ((int)type < 0 || (int)type > 2) return HIDP_STATUS_INVALID_REPORT_TYPE;
    if (rlen != pd->Lengths[type]) return HIDP_STATUS_INVALID_REPORT_LENGTH;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++)
        if (f[i].Type == (UCHAR)type && f[i].ReportId == id) { exists = 1; break; }
    if (!exists) return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    HpZero(report, rlen);
    report[0] = (CHAR)id;
    return HIDP_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the rest of the HidP API (used by HID.DLL; HIDPARSE.SYS exports it too) */
/* ------------------------------------------------------------------ */

/* usages that left (break) and that came (make) between two lists, 0-terminated */
NTSTATUS __stdcall HidP_UsageListDifference(
    IN PUSAGE prev, IN PUSAGE cur, OUT PUSAGE brk, OUT PUSAGE make, IN ULONG len)
{
    ULONG i, j, b = 0, m = 0;

    for (i = 0; i < len && prev[i]; i++) {
        for (j = 0; j < len && cur[j]; j++)
            if (cur[j] == prev[i]) break;
        if (j == len || !cur[j]) brk[b++] = prev[i];
    }
    for (i = 0; i < len && cur[i]; i++) {
        for (j = 0; j < len && prev[j]; j++)
            if (prev[j] == cur[i]) break;
        if (j == len || !prev[j]) make[m++] = cur[i];
    }
    while (b < len) brk[b++] = 0;
    while (m < len) make[m++] = 0;
    return HIDP_STATUS_SUCCESS;
}

/* like HidP_GetUsages, over all usage pages: usage and its page */
NTSTATUS __stdcall HidP_GetUsagesEx(
    IN HIDP_REPORT_TYPE type, IN USHORT link, OUT PUSAGE_AND_PAGE list,
    IN OUT PULONG len, IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    ULONG i, k, n = 0;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = PD_FIELDS(pd);
    for (i = 0; i < pd->NumFields; i++) {
        if (f[i].Type != (UCHAR)type || f[i].ReportId != id || !IsButtonField(&f[i])) continue;
        if (link && f[i].Node != link) continue;
        for (k = 0; k < f[i].Count; k++) {
            USAGE u = 0;
            if (f[i].Kind == FK_BUTTON) {
                if (GetBits(data, f[i].BitPos + k * f[i].BitSize, 1))
                    u = (USAGE)(f[i].UsageMin + k);
            } else {
                u = ArrayUsage(&f[i], pd,
                               (LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize));
            }
            if (u) {
                if (n < *len) { list[n].Usage = u; list[n].UsagePage = f[i].Page; }
                n++;
            }
        }
    }
    if (n > *len) { *len = n; return HIDP_STATUS_BUFFER_TOO_SMALL; }
    *len = n;
    return HIDP_STATUS_SUCCESS;
}

/* the physical value is scaled back to the logical range of the field */
NTSTATUS __stdcall HidP_SetScaledUsageValue(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    IN LONG value, IN PHIDP_PREPARSED_DATA pd, IN OUT PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    LONG v;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    if (f->PhysMin == 0 && f->PhysMax == 0) {
        v = value;                                  /* no physical range: logical = physical */
        if (v < f->LogMin || v > f->LogMax) goto out_of_range;
    } else {
        if (f->PhysMax <= f->PhysMin || f->LogMax <= f->LogMin)
            return HIDP_STATUS_BAD_LOG_PHY_VALUES;
        if (value < f->PhysMin || value > f->PhysMax) goto out_of_range;
        v = Rescale(value, f->PhysMin, f->PhysMax, f->LogMin, f->LogMax);
    }
    PutBits(data, f->BitPos + (ULONG)(usage - f->UsageMin) * f->BitSize, f->BitSize, (ULONG)v);
    return HIDP_STATUS_SUCCESS;

out_of_range:
    if (f->Flags & MF_NULLSTATE) {                  /* write the null state, below LogMin */
        PutBits(data, f->BitPos + (ULONG)(usage - f->UsageMin) * f->BitSize, f->BitSize,
                (ULONG)(f->LogMin - 1));
        return HIDP_STATUS_NULL;
    }
    return HIDP_STATUS_VALUE_OUT_OF_RANGE;
}

/* a value that spans several fields (one usage, Count > 1): the raw bits, LSB first */
NTSTATUS __stdcall HidP_GetUsageValueArray(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    OUT PCHAR out, IN USHORT outlen, IN PHIDP_PREPARSED_DATA pd, IN PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    ULONG bits, i;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    if (f->Count < 2) return HIDP_STATUS_NOT_VALUE_ARRAY;
    bits = (ULONG)f->Count * f->BitSize;
    if (outlen < (bits + 7) / 8) return HIDP_STATUS_BUFFER_TOO_SMALL;
    HpZero(out, (bits + 7) / 8);
    for (i = 0; i < bits; i++)
        if (GetBits(data, f->BitPos + i, 1))
            out[i / 8] |= (CHAR)(1 << (i % 8));
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS __stdcall HidP_SetUsageValueArray(
    IN HIDP_REPORT_TYPE type, IN USAGE page, IN USHORT link, IN USAGE usage,
    IN PCHAR in, IN USHORT inlen, IN PHIDP_PREPARSED_DATA pd, OUT PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    HP_FIELD *f;
    ULONG bits, i;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = FindValueField(pd, type, page, link, usage, id, &st);
    if (!f) return st;
    if (f->Count < 2) return HIDP_STATUS_NOT_VALUE_ARRAY;
    bits = (ULONG)f->Count * f->BitSize;
    if (inlen < (bits + 7) / 8) return HIDP_STATUS_BUFFER_TOO_SMALL;
    for (i = 0; i < bits; i++)
        PutBits(data, f->BitPos + i, 1, (in[i / 8] >> (i % 8)) & 1);
    return HIDP_STATUS_SUCCESS;
}

/* the data index range of a field (what HidP_GetData numbers its entries with) */
static ULONG DataIndexHigh(const HP_FIELD *f)
{
    if (f->Kind == FK_BUTTON) return f->DataIndexMin + (f->Count ? f->Count - 1 : 0);
    if (f->Kind == FK_ARRAY)
        return f->DataIndexMin + (f->ListCount ? f->ListCount - 1 : (ULONG)(f->UsageMax - f->UsageMin));
    return f->DataIndexMin;
}

/* the opposite of HidP_GetData: entries are set, nothing is cleared */
NTSTATUS __stdcall HidP_SetData(
    IN HIDP_REPORT_TYPE type, IN PHIDP_DATA list, IN OUT PULONG len,
    IN PHIDP_PREPARSED_DATA pd, IN OUT PCHAR report, IN ULONG rlen)
{
    PUCHAR data; UCHAR id;
    ULONG e, i, k;
    HP_FIELD *f;
    NTSTATUS st = CheckReport(pd, type, report, rlen, &data, &id);

    if (!NT_SUCCESS(st)) return st;
    f = PD_FIELDS(pd);
    for (e = 0; e < *len; e++) {
        ULONG idx = list[e].DataIndex;
        int done = 0;

        for (i = 0; i < pd->NumFields && !done; i++) {
            if (f[i].Type != (UCHAR)type || f[i].ReportId != id) continue;
            if (idx < f[i].DataIndexMin || idx > DataIndexHigh(&f[i])) continue;
            if (f[i].Kind == FK_BUTTON) {
                k = idx - f[i].DataIndexMin;
                PutBits(data, f[i].BitPos + k * f[i].BitSize, 1, list[e].On ? 1 : 0);
                done = 1;
            } else if (f[i].Kind == FK_ARRAY) {
                LONG want = (LONG)(idx - f[i].DataIndexMin) + f[i].LogMin;
                if (!list[e].On) { done = 1; continue; }
                for (k = 0; k < f[i].Count; k++)
                    if ((LONG)GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize) == want)
                        break;
                if (k == f[i].Count)
                    for (k = 0; k < f[i].Count; k++)
                        if (GetBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize) == 0) {
                            PutBits(data, f[i].BitPos + k * f[i].BitSize, f[i].BitSize, (ULONG)want);
                            break;
                        }
                if (k == f[i].Count) { *len = e; return HIDP_STATUS_BUFFER_TOO_SMALL; }
                done = 1;
            } else {
                PutBits(data, f[i].BitPos, f[i].BitSize, list[e].RawValue);
                done = 1;
            }
        }
        if (!done) { *len = e; return HIDP_STATUS_USAGE_NOT_FOUND; }
    }
    return HIDP_STATUS_SUCCESS;
}
