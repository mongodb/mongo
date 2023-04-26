import { initialState } from "./store";

export const listSearchTerm = (state = initialState, action) => {
  switch (action.type) {
    case "setListSearchTerm":
      return action.payload;

    default:
      return state;
  }
};

export const setListSearchTerm = (listSearchTerm) => ({
  type: "setListSearchTerm",
  payload: listSearchTerm,
});
