#pragma once

class HtArrayIterator {
public:
	explicit HtArrayIterator(const dart::Array& data_) : data(data_), max_idx(data_.Length() - 1), curr_idx(1) {}
	HtArrayIterator() = delete;

	uint32_t Size() const {
		const auto& num_used = dart::Smi::Cast(dart::Object::Handle(data.At(0)));
		return (uint32_t)num_used.Value();
	}

	bool MoveNext() {
		while (curr_idx < max_idx) {
			++curr_idx;
			auto objPtr = data.At(curr_idx);
			if (objPtr.GetClassId() != dart::kSentinelCid)
				return true;
		}
		return false;
	}

	dart::ObjectPtr Current() const {
		return data.At(curr_idx);
	}

	intptr_t CurrentIndex() const {
		return curr_idx;
	}

private:
	const dart::Array& data;
	const intptr_t max_idx;
	intptr_t curr_idx;
};
